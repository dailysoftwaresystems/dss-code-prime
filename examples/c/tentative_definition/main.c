// c33 D-CSUBSET-TENTATIVE-DEFINITION witness: a file-scope object declared WITHOUT
// an initializer is a TENTATIVE DEFINITION (C 6.9.2). Any number of tentatives plus
// AT MOST ONE real (initialized) definition of the same name MERGE into ONE object;
// two tentatives alone merge into a single zero-initialized object. Pre-fix this
// collided S_RedeclaredSymbol (a tentative was treated as a full definition). This
// is the sqlite3.c frontier shape (`u32 sqlite3WhereTrace; ... u32 sqlite3WhereTrace
// = 0;`).
//
// Covers, in ONE program, every merge shape the fix introduces:
//   * tentative THEN real definition   `int g; ... int g = 40;`  (def wins its init)
//   * real definition THEN tentative   `int d = 2; ... int d;`   (reverse order)
//   * two tentatives only              `int z; int z;`           (=> zero-init)
// and reads each through a NON-INLINED function (`load_*`) so the merged storage is
// exercised at runtime across a call boundary (not const-folded away).
//
// Value-divergent:
//   g = 40 (def's init survives the merge)
//   d = 2  (def's init survives regardless of tentative ordering)
//   z = 0  (two tentatives => single zero-initialized object)
//   exit = load_g() + load_d() + load_z() = 40 + 2 + 0 = 42
// Any merge bug (wrong survivor, dropped init, or a spurious second object) =>
// wrong exit (or a compile failure if the merge regresses to S_RedeclaredSymbol).
//
// RED-ON-DISABLE: drop `isTentativeDefinition` from the Pass-1 `newNonDef` fold (the
// semantic_analyzer.cpp variable bind) -> `int g; int g = 40;` collide
// S_RedeclaredSymbol -> the program no longer COMPILES (the runner reports a compile
// failure instead of exit 42).
//
// Front-end feature (semantic merge; HIR emits exactly one global per name with the
// right init), target- and format-agnostic. Runs on x86_64 (PE+ELF) and arm64
// (ELF qemu, Mach-O macos leg). The release arm re-witnesses under the shipped
// pipeline (one ordinary global per name; the absorbed tentative emits no node).

int g;                 // tentative definition of g (no initializer)
int d = 2;             // REAL definition of d (initialized)
int z;                 // tentative definition of z
int z;                 // second tentative of z (merges; object stays zero-init)

int g = 40;            // REAL definition of g (merges; provides the init 40)
int d;                 // redundant tentative of d AFTER its definition (reverse)

// Non-inlined readers: a separate translation-unit-internal call boundary forces the
// global loads to happen at runtime against the merged storage.
int load_g(void) { return g; }
int load_d(void) { return d; }
int load_z(void) { return z; }

int main(void) {
    return load_g() + load_d() + load_z();   // 40 + 2 + 0 = 42
}
