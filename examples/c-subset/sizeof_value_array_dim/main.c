// D-FC6-SIZEOF-ARRAY-DIM-VALUE-FORM ✅ + D-SEMANTIC-SUBTREETYPE-TRANSPARENT-
// WRAPPERS ✅: `sizeof` of an EXPRESSION (the value form) in an array dimension
// now folds — the classic element-count idiom `sizeof(b)/sizeof(b[0])`.
//
// This works because the complete semantic-tier expression typer derives the
// operand types at Pass 1.5 (before the array type is interned):
//   sizeof(b)     — `b` is an identifier → its symbol type `int[8]` → 32 bytes
//   sizeof(b[0])  — `b[0]` is an Index over `b` → element type `int` → 4 bytes
//   so a's dimension = 32 / 4 = 8  →  `a` is interned as `int[8]`.
//
// `+ x` keeps the exit RUNTIME (x is a function arg); if the value-form operands
// mis-typed (e.g. the old suppressor returning the array type for `b[0]`), the
// dimension would be wrong (or fail loud) and the exit would drift off 42.
//   exit = 34 + (sizeof(a) / sizeof(int)) = 34 + 32/4 = 34 + 8 = 42.

int run(int x) {
  int b[8];
  int a[sizeof(b) / sizeof(b[0])];   // VALUE form in the dimension
  return x + (int)(sizeof(a) / sizeof(int));
}

int main(void) { return run(34); }
