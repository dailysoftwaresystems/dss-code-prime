// D-CSUBSET-FN-PROTOTYPE (negative): two declarations of `f` with INCOMPATIBLE
// signatures must fail loud (C 6.7p4 / 6.9.1), never silently pick one. The
// prototype declares `int f(int)`; the definition declares `int f(char *)`.
// Their interned FnSigs differ -> S_IncompatibleRedeclaration, positioned at the
// absorbed (prototype) declaration with a related-location at the survivor.
int f(int);
int f(char *x) { return 0; }
int main(void) { return 0; }
