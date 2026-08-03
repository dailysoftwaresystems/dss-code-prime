// D-CSUBSET-GNU-ATTRIBUTE (TF-C72/C73) witness, CU B: the sibling translation
// unit. It carries NO attributes at all, on purpose — every definition here is
// STRONG, and that is what makes cu_a.c's `weak` declarations observable.
//
// `gg` and `wp` are the LINK TARGETS for cu_a.c's two audit-named prototypes
// (the glibc `__nothrow__,__leaf__` idiom, and a `weak` prototype whose body
// lives in another TU). Neither is weak here; cu_a.c simply has to be able to
// declare and call them.
//
// `wfun`, `wd_lead` and `wd_tail` are the STRONG halves of three strong-over-weak
// pairs. cu_a.c defines each of them WEAKLY with the value 100; the linker must
// discard those and keep these, so the program observes 12 / 5 / 5. If a `weak`
// attribute in cu_a.c were parsed but its binding never reached the symbol, the
// resolution would go the other way and cu_a.c's `main` would exit 3, 4 or 5
// instead of 42 — which is exactly the regression these pairs exist to catch.
// If a `weak` were DELETED from cu_a.c instead, this file's definition collides
// with it and the build fails loud: DSS reports `K_SymbolRedefinedAcrossUnits`,
// clang's linker reports `duplicate symbol`. Both were MEASURED.
//
// Do NOT add `__attribute__((weak))` to anything in this file. Two weak
// definitions of the same symbol make the winner an arbitrary linker choice
// rather than a language rule, and the checks in cu_a.c would stop meaning
// anything.

int gg(void) { return 10; }
int wp(void) { return 8; }

int wfun(void) { return 12; }
int wd_lead = 5;
int wd_tail = 5;
