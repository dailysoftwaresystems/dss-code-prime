// C 6.2.3 TAG NAMESPACE — closes the tag-namespace residue of
// D-CSUBSET-DECL-GRAMMAR-LOW-RESIDUES. The struct/union/enum TAG namespace
// is SEPARATE from the ordinary-identifier namespace, so a tag name and a
// typedef alias may share a spelling:
//
//   typedef struct Pair { int a; int b; } Pair;
//                  ^^^^                    ^^^^
//                  tag Pair (Tag ns)       typedef Pair (Ordinary ns)
//
// Before this change the single-namespace symbol table rejected this with
// S_RedeclaredSymbol. This compile exercises BOTH namespaces in ONE TU:
//   * `Pair p;`        — the ALIAS, resolved Ordinary via the type-position
//                        Token-leaf arm.
//   * `struct Pair q;` — the TAG, resolved Tag via the type-position
//                        tag-reference early-arm (MF-1).
// Both must resolve to the SAME struct type, so the field arithmetic yields
// 42 — proving the two namespaces resolve independently to one type.
//
// RED-ON-DISABLE: without the Tag namespace the `typedef` line fails the
// compile loud (S_RedeclaredSymbol on `Pair`); the example never links.
typedef struct Pair { int a; int b; } Pair;

int main(void) {
    Pair p;
    p.a = 40;
    p.b = 2;
    struct Pair q;
    q.a = p.a;
    q.b = p.b;
    return q.a + q.b;   // 40 + 2 = 42
}
