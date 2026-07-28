// D-CSUBSET-ATTRIBUTE-LEADING-WITH-STORAGE-CLASS (TF-C77) witness, CU B: the
// sibling translation unit. It carries NO attributes at all, on purpose — the
// single definition here is STRONG, and that is what makes cu_a.c's mid-position
// `weak` observable as an EXIT CODE rather than merely as "it compiled".
//
// cu_a.c defines `wd` WEAKLY with the value 100; the linker must discard that
// and keep this one, so the program observes 5. If the mid-position `weak` were
// parsed but its binding never reached the symbol — which is exactly the H2
// halfway state, `linkagePrefixRoots` without its slot roots — resolution would
// go the other way and cu_a.c's `main` would exit 2 instead of 42. If the `weak`
// were DELETED from cu_a.c instead, this definition collides with it and the
// build fails loud: DSS reports K_SymbolRedefinedAcrossUnits, clang's linker
// reports `duplicate symbol`. Both were MEASURED.
//
// Do NOT add `__attribute__((weak))` here. Two weak definitions of one symbol
// make the winner an arbitrary linker choice rather than a language rule, and
// the check in cu_a.c would stop meaning anything.

int wd = 5;
