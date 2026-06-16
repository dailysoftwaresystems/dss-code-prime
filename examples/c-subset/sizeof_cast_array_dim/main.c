// D-SEMANTIC-SUBTREETYPE-TRANSPARENT-WRAPPERS regression (review C1): a CAST in
// a `sizeof` operand RE-TYPES — `sizeof((char)y)` is sizeof(char)=1, NOT
// sizeof(y)'s int=4. The Pass-1.5 typer must type the cast wrapper by its
// target type, not descend past it to the operand. `(char)` is dataModel-
// invariant (char is 1 byte on every target), so this witness is exact on all
// four targets:
//   sizeof((char)y) = 1  →  `a` is interned as `int[1]`  →  sizeof(a)/sizeof(int) = 1
//   exit = 41 + 1 = 42.
// RED-ON-DISABLE: drop the cast arm and `subtreeType` descends to `y` (int),
// folding `int[4]` → exit 45.

int run(int x) {
  int y;
  int a[sizeof((char)y)];
  return x + (int)(sizeof(a) / sizeof(int));
}

int main(void) { return run(41); }
