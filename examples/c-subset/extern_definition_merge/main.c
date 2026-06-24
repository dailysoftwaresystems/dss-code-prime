// D-CSUBSET-EXTERN-DEFINITION-MERGE witness: an `extern` declaration followed (or
// preceded) by a same-TU DEFINITION of the same name MERGES — the definition
// satisfies the extern (like the prototype/definition "definition wins" case),
// and the extern's import row is suppressed (no spurious duplicate ExternGlobal/
// ExternFunction). Pre-fix this collided S_RedeclaredSymbol (the extern routed
// through a different machine than the bare-proto merge).
//
// Covers BOTH forms in one program:
//   * extern OBJECT  `extern int g;` + `int g = 30;`  (definition provides storage)
//   * extern FUNCTION `extern int add(int,int);` + its body (def-after-extern)
// plus a def-before-extern object to exercise the reverse order.
//
// All defined locally (no real cross-TU import), so the program links and runs as
// ONE unit. Value-divergent:
//   g = 30 ; add(g, 12) = 42  => exit 42 ; any merge/suppress bug => wrong exit.
//
// RED-ON-DISABLE: revert the Pass-1 extern merge (drop `nonDefiningDeclaration` /
// the mergeOrCollideRedeclaration extern arm) -> `extern int g;` + `int g = 30;`
// collide S_RedeclaredSymbol, the program no longer COMPILES (the runner reports a
// compile failure instead of exit 42).
//
// Front-end feature (semantic merge + HIR extern-node suppression), target- and
// format-agnostic. Runs on x86_64 (PE+ELF) and arm64 (ELF qemu, Mach-O macos leg).

extern int g;                       // extern OBJECT declaration (non-defining)
extern int add(int a, int b);       // extern FUNCTION declaration (non-defining)

int g = 30;                         // DEFINITION of g (merges; provides storage)
int add(int a, int b) { return a + b; }   // DEFINITION of add (merges with extern)

int h = 0;                          // DEFINITION of h, BEFORE its extern below
extern int h;                       // redundant extern AFTER the def (reverse order)

int main(void) {
    return add(g, 12) + h;          // 30 + 12 + 0 = 42
}
