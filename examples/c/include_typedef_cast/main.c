// FC2 (V2-4.X, 2026-06-10): the type-name ORACLE runtime witness. The
// typedef `myint` is ONLY visible via the quote include — main.c's
// first parse sees `(myint)-x` as a lone UNKNOWN identifier with an
// operator follower (triage rule 4), rolls back to the value reading,
// and records an AmbiguousTypeNameCandidate. UnitBuilder::finish()
// resolves `myint` against the header tree's exported global type
// names and REPARSES this file once with the name seeded — only then
// does the cast commit and the program compile clean.
//
//   negate(-42) = (myint)-(-42) = 42  ->  exit 42
//
// An oracle regression (candidate not recorded / export surface not
// harvested / reparse not run) leaves the value reading in place ->
// `myint` is an undeclared VALUE operand -> S_UndeclaredIdentifier ->
// the harness's zero-diagnostic compile assert flips RED.
//
// Identity typedef cast + pure int ops -> all four target arms (the
// modulo arm set). The runtime exit additionally proves the reparsed
// tree (not the rolled-back first parse) is what reached codegen.
#include "myint.h"

int negate(int x) { return (myint)-x; }

int main() { return negate(-42); }
