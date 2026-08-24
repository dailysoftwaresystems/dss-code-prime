// D-PARSE-DEEP-NEST-RECURSION-MEMORY (plan 24 Stage 1): a DEEP pin for the
// iterative (explicit-work-stack) semantic expression typer subtreeType.
//
// `char a[sizeof(<200 nested parens around ll>)]` folds at Pass 1.5 via
// subtreeType, which derives typeof on the UNSTAMPED CST (Pass 2 has not yet
// stamped the array-dimension operand). The 200-deep transparent-wrapper chain
// drives subtreeType's Wrapper arm 200 levels into its explicit heap work-stack
// (NOT the host stack), bottoming out at `ll` (long long) -> typeof == long long
// -> sizeof == 8 -> `a` is char[8] -> run(34) == 34 + 8 == 42. A mis-typed deep
// descent would make sizeof 4 (int) and the exit drift to 38, or fail to fold.
// Depth 200 is ~50x the deepest expression in the rest of the corpus (<=4) and
// exercises the work-stack driver far past any shallow output-identity test;
// long long is 8 bytes on every data model, so the exit is 42 on all targets.
// (Pure parens keep the PARSE fast — deep binary nesting is parser-bound until
// the parser itself goes iterative in a later stage.)

int run(int x) {
  long long ll = 2;
  char a[sizeof( ((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((ll)))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))) )];
  return x + (int)sizeof(a);
}

int main(void) { return run(34); }
