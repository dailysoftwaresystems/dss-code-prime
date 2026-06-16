// R2 (D-SEMANTIC-NULL-CONSTANT-FOLDING): a FOLDED-zero integer constant
// expression (`1 - 1`) passed where a pointer is expected is a C null pointer
// constant (C §6.3.2.3p3) — it must compile to a NULL pointer, not a type error
// and not garbage. `f` is non-inlined, so the pointer value genuinely flows
// through codegen / parameter-passing: a correct NULL yields 42, while a
// mis-materialized non-null value would yield 7 — a loudly different exit
// (42 vs 7, mod-256-safe). Red-on-disable: revert R2 → `f(1 - 1)` fails to
// COMPILE (S_TypeMismatch) → this example fails to build at all.
int f(int *p) { return p ? 7 : 42; }
int main(void) { return f(1 - 1); }
