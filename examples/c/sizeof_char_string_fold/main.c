/* R1 (crash fix) + R2 (fold): a character constant has type `int` (C 6.4.4.4),
 * so sizeof('c') == 4 (NOT sizeof(char)==1); a string literal has type char[N+1]
 * (C 6.4.5), so sizeof("abcd") == 5. Both fold at the semantic/MIR tier (the
 * sizeof operand is UNEVALUATED). The exit code reflects the folded sizes:
 * 4 + 5 + 33 = 42 — a wrong fold (e.g. sizeof('c')==1) would give a loudly
 * different exit. PRE-FIX, (int)sizeof('c') HARD-CRASHED the compiler
 * (`TypeInterner::get: TypeId out of range` — an unguarded interner.kind() on the
 * failed-sizeof's InvalidType operand in lowerCast). Red-on-disable: revert the
 * char/string literal typing and this fails to COMPILE (H0009 sizeof-unresolved). */
int main(void) {
    return (int)sizeof('c') + (int)sizeof("abcd") + 33;
}
