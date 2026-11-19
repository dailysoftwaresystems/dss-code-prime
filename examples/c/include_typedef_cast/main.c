// FC2 (V2-4.X, 2026-06-10): a QUOTE-INCLUDED typedef commits a cast on the
// file's one parse. The typedef `myint` is visible ONLY through the quote
// include; the C preprocessor splices the header's text in before the parse
// (FC13), so the binder sketch knows `myint` as a type when `(myint)-x` is
// read and the cast commits. (Before P69 a compilation-unit oracle reparsed
// the file with names other trees exported; it is retired —
// D-PP-FC2-ORACLE-RETIRE — and a C file parses once.)
//
//   negate(-42) = (myint)-(-42) = 42  ->  exit 42
//
// A regression (the header's text not spliced before the parse, or its
// typedef not bound in the sketch) leaves the value reading in place ->
// `myint` is an undeclared VALUE operand -> S_UndeclaredIdentifier ->
// the harness's zero-diagnostic compile assert flips RED.
//
// Identity typedef cast + pure int ops -> all four target arms (the
// modulo arm set). The runtime exit proves the committed cast is what
// reached codegen.
#include "myint.h"

int negate(int x) { return (myint)-x; }

int main() { return negate(-42); }
