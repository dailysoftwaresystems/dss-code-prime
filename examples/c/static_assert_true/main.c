// C11/C23 6.7.10 (D-CSUBSET-STATIC-ASSERT): a PASSING static assertion whose
// condition folds `sizeof(int)==4` — the single most common idiom, and the one
// that pins the sizeof-folding requirement end-to-end. A true assertion produces
// NOTHING (no HIR, no code); the program compiles and runs to exit 42.
_Static_assert(sizeof(int) == 4, "int is 4 bytes");

int main(void) {
    int v = 0;
    // The VALUE form `sizeof v` (not just the TYPE form `sizeof(T)`) must also
    // fold, at block scope — a distinct const-eval path (the subtreeType stamp
    // rather than type-node resolution). A regression pin for the sizeof fix.
    // `int` is 4 bytes under BOTH LP64 (elf) and LLP64 (pe64) — data-model-
    // independent, unlike `long` (8 vs 4).
    _Static_assert(sizeof v == 4, "int is 4 bytes");
    return v + 42;
}
