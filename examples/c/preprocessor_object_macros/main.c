// FC13 cycle 1 -- the C-preprocessor object-macro + quote-`#include` runtime
// witness. Exercises, end to end through every target leg:
//   * a quote-`#include "consts.h"` whose `#define`s (BASE, MARGIN) reach here
//     ONLY because the preprocessor splices the header text in BEFORE parsing
//     (the retired post-parse include-following arm could not deliver a macro);
//   * a local object-like `#define SUM` whose replacement is itself a macro
//     expression (BASE + MARGIN) -- proving expansion + RESCAN;
//   * a `#define BONUS 9` + `#undef BONUS` + re-`#define BONUS 0`, proving
//     `#undef` actually removes the binding: if `#undef` were a no-op the
//     incompatible-redefinition guard would fire (a compile diagnostic) AND the
//     value 9 would leak, so the exit code would be 51, not 42.
//
// Fold-resistance: the macro-expanded operands reach `combine` as FUNCTION
// ARGUMENTS, so the baseline (unoptimized) arm keeps a live runtime add -- the
// computation is not const-folded into a single immediate at exit. The PRIMARY
// witness is still compile-time: a dropped macro expansion leaves an undefined
// identifier and the program never links.
//
//   SUM = BASE + MARGIN = 30 + 12 = 42; combine(42, 0) = 42 + 0 = 42 -> exit 42
#include "consts.h"

#define SUM (BASE + MARGIN)

#define BONUS 9
#undef BONUS
#define BONUS 0

int combine(int a, int b) {
    return a + b;
}

int main(void) {
    return combine(SUM, BONUS);
}
