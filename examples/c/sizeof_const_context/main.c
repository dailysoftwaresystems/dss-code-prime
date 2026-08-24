// FC6 deferral-close (D-FC6-SIZEOF-CONST-CONTEXT): `sizeof` now const-folds in
// const-expression contexts, not just in a runtime expression. This one binary
// exercises all three folding sites at once:
//
//   (1) GLOBAL INITIALIZER  — `int g = (int)sizeof(struct Pair);`
//        folds at module-load time through the HIR const-eval engine.
//   (2) ARRAY DIMENSION     — `int row[sizeof(struct Pair)];`
//        the length folds at SEMANTIC time (the CST const-eval engine), so
//        `row` is `int[8]` — interned as a real array type.
//   (3) EXPRESSION sizeof   — `sizeof(row)` / `sizeof(int)` in `run`
//        fold at MIR time (the FC6 expression-context sizeof).
//
//   struct Pair { int a; int b; };   → size 8
//   g                                = 8
//   sizeof(row) / sizeof(int)        = (8 * 4) / 4 = 8   (row is int[8])
//   exit = x + g + 8 - 8 = x + 8 = 34 + 8 = 42
//
// `run`'s `x` arrives as a function arg, so ConstFold cannot collapse the whole
// program to a literal — yet every sizeof above folds. If ANY of the three
// folds the wrong size (mis-padded struct, a dropped array dimension, a wrong
// element stride), the exit drifts off 42 — the witness is layout-sensitive.

struct Pair { int a; int b; };

int g = (int)sizeof(struct Pair);

int run(int x) {
  int row[sizeof(struct Pair)];
  return x + g + (int)(sizeof(row) / sizeof(int)) - 8;
}

int main(void) { return run(34); }
