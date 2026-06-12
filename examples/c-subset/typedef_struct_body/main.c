// FC4 c1 stage 2b — typedef-position struct BODIES (C 6.7.2.1
// structSpecifierBody): tagged and ANONYMOUS composite definitions
// inside a typedef head, with the minted aliases then used in
// DECLARATION position as pointer declarators (struct VALUES at
// runtime stay out of scope — scalars carry the exit code).
//
//   * `typedef struct PairTag { int a; int b; } Pair;` — tagged body;
//     the tag and the alias are DISTINCT names because the symbol
//     table is single-namespace today (a same-named
//     `typedef struct Pair {...} Pair;` is S_RedeclaredSymbol — C's
//     separate tag namespace is pinned registry residue).
//   * `typedef struct { int z; } Anon;` — ANONYMOUS body: only the
//     alias is minted.
//
// Exit arithmetic: base 40 + 1 (pp null-check) + 1 (ap null-check)
// = 42; each broken pointer init shifts by 1 (!= 0 mod 256).
typedef struct PairTag { int a; int b; } Pair;
typedef struct { int z; } Anon;

int main() {
    Pair *pp = 0;
    Anon *ap = 0;
    int base = 40;
    if (pp == 0) { base = base + 1; }
    if (ap == 0) { base = base + 1; }
    return base;
}
