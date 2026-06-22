// FC14 (D-PP-CONDITIONAL-COMPILATION) -- the conditional-compilation runtime
// witness. Exercises, end to end through every target leg, that the
// preprocessor's #if/#ifdef/#ifndef/#elif/#else/#endif + `defined` select the
// RIGHT branch and ELIDE the wrong ones:
//
//   * `#define MODE 2` + a `#if MODE == 1 / #elif MODE == 2 / #else` chain whose
//     #elif branch is the one taken (MODE == 2) -- so `BASE` is #define'd to 40,
//     NOT 10 (the #if branch) and NOT 0 (the #else). A wrong-branch selection
//     changes BASE and the exit code moves off 42.
//   * a `#if 0 ... #endif` block containing `return 99;` that MUST be elided:
//     if the dead branch were kept, `main` would `return 99` before reaching the
//     real return, so the exit code would be 99, not 42.
//   * `#ifdef BASE` proves the #elif branch's `#define BASE` actually took
//     effect (an undefined BASE would take the #else here and use the wrong
//     MARGIN); `#ifndef MISSING` + `#if defined(MODE)` exercise the no-paren and
//     paren `defined` forms and #ifndef.
//
// Fold-resistance (mirrors the array-storage witness): BASE and MARGIN reach
// `add` as FUNCTION ARGUMENTS, so the baseline (unoptimized) arm keeps a live
// runtime add -- the result is not const-folded to a single immediate at exit.
// The optimizedPipelines `release` arm runs the shipped pipeline over the SAME
// source. The PRIMARY witness is still compile-time: a wrong elision either
// leaves `return 99` live (exit 99) or leaves an undefined identifier (link
// failure).
//
//   MODE == 2 -> BASE = 40; #ifdef BASE -> MARGIN = 2; add(40, 2) = 42 -> exit 42

#define MODE 2

#if MODE == 1
#define BASE 10
#elif MODE == 2
#define BASE 40
#else
#define BASE 0
#endif

#if 0
// DEAD branch: if conditional elision regressed, this `return 99` would run
// first and the program would exit 99 instead of 42.
int dead_should_not_compile(void) { return 99; }
#endif

#ifdef BASE
#define MARGIN 2
#else
#define MARGIN 99
#endif

// Exercise the `defined` operator in both the no-paren and paren forms, plus
// #ifndef, all of which must be TRUE here (MODE is defined, MISSING is not).
#ifndef MISSING
#if defined MODE
#if defined(BASE)
#define OK 1
#endif
#endif
#endif

#ifndef OK
#define MARGIN 99
#endif

int add(int a, int b) {
    return a + b;
}

int main(void) {
    return add(BASE, MARGIN);
}
