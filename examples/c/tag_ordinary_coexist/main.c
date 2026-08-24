// C 6.2.3 TAG vs ORDINARY namespace COEXISTENCE — the independence proof for
// the tag-namespace residue of D-CSUBSET-DECL-GRAMMAR-LOW-RESIDUES.
//
// A struct TAG `S` (Tag namespace) and an ordinary OBJECT `S` (Ordinary
// namespace) carry the SAME spelling in the same scope chain and do NOT
// collide. Each is read through its own namespace:
//   * `struct S a;` — the TAG, via the type-position tag-reference arm.
//   * bare `S`      — the ORDINARY local int, via ordinary identifier
//                     resolution.
// They resolve to entirely different entities (a struct type vs an int
// object), which is exactly the point: the namespaces are independent.
//
// Exit arithmetic: a.v (40) + S (2) = 42.
// RED-ON-DISABLE: in a single-namespace table the local `int S` would
// collide with the tag `S` (S_RedeclaredSymbol) and the compile fails loud.
struct S { int v; };

int main(void) {
    struct S a;
    a.v = 40;
    int S = 2;            // ordinary object `S` — coexists with tag `S`
    return a.v + S;       // 40 + 2 = 42
}
