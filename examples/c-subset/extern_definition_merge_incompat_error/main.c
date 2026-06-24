// D-CSUBSET-EXTERN-DEFINITION-MERGE (negative): an `extern` declaration and a
// same-TU DEFINITION with an INCOMPATIBLE type must fail loud (C 6.7p4), never
// silently pick one. The extern declares `int g`; the definition declares
// `long g`. Their interned types differ -> S_IncompatibleRedeclaration, positioned
// at the absorbed extern declaration, with a related-location at the surviving
// definition.
//
// This is the SAME compatibility sweep the prototype/definition merge uses,
// extended across the extern<->definition boundary — the merge admits an extern +
// a definition only when their types match.
extern int g;
long g = 5;
int main(void) { return 0; }
