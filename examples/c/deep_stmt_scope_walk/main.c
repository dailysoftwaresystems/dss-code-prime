// D-PARSE-DEEP-NEST-RECURSION-MEMORY (plan 24 Stage 2): a DEEP pin for the
// iterative (explicit-work-stack) semantic WHOLE-TREE walkers pass1 + pass2.
//
// 150 nested block scopes. pass1 (PRE-order) pushes a scope per block, building
// a 150-deep scope parent chain; pass2 (POST-order) then walks the tree, and the
// innermost `return base` resolves `base` by climbing that 150-deep chain back to
// the outermost scope. Both walkers run through their explicit heap work-stacks
// 150 levels deep (NOT the host stack). The exit is value-sensitive: `base` must
// resolve through all 150 scopes for the program to both COMPILE and return 42 —
// a broken deep scope tree (wrong pass1 push order or pass2 walk) would fail to
// resolve `base`. Depth 150 is ~37x the deepest nesting elsewhere in the corpus
// (<=4), exercising the work-stack drivers far past any shallow output-identity
// test. (Nested blocks parse O(N) — no expression speculation — so this compiles
// fast even while the parser itself is still recursive.)

int run(void) {
  int base = 42;
  { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { { return base; } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } }
}

int main(void) { return run(); }
