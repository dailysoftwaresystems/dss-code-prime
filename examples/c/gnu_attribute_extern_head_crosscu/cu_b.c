// D-CSUBSET-ATTRIBUTE-LEADING-WITH-STORAGE-CLASS (TF-C77) witness, CU B: the
// sibling translation unit. It carries NO attributes at all, on purpose — every
// definition here is STRONG, and that is what makes cu_a.c's `weak` declaration
// observable.
//
// `wfun` is the STRONG half of the one strong-over-weak pair. cu_a.c defines it
// WEAKLY returning 100; the linker must discard that and keep this one, so the
// program observes 12. If cu_a.c's `weak` were parsed but its binding never
// reached the symbol, resolution would go the other way and `main` would exit 1
// instead of 42. If the `weak` were DELETED from cu_a.c instead, this definition
// collides with it and the build fails loud — DSS reports
// K_SymbolRedefinedAcrossUnits, clang's linker reports `duplicate symbol`. Both
// were MEASURED.
//
// `die` is a REAL non-returning function (an infinite loop). It must genuinely
// never return: cu_a.c declares it `__noreturn__` and the compiler is entitled
// to treat the path after a call to it as unreachable, so a `die` that returned
// would be undefined behavior rather than a test of anything.
//
// Do NOT add `__attribute__((weak))` to anything in this file. Two weak
// definitions of the same symbol make the winner an arbitrary linker choice
// rather than a language rule, and the checks in cu_a.c would stop meaning
// anything.

int wfun(void) { return 12; }
int gg(void)   { return 10; }
int ev = 7;

_Thread_local int t_first  = 5;
_Thread_local int t_second = 6;

void die(int c) { while (c >= 0) { } }
