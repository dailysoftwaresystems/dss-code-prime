/* c24 IDENTITY-SAFETY WITNESS (D-CSUBSET-SELF-REFERENTIAL-STRUCT): two same-name
 * structs with DIFFERENT layouts in different block scopes of ONE translation
 * unit. Nominal composite identity is keyed by (kind, name, DECL-SITE) — NOT
 * name-only — so these are DISTINCT types with INDEPENDENT field offsets.
 *
 * file-scope `struct S { int a; }`           → size 4, a@0
 * block-scope `struct S { int x,y,z; }`      → size 12, x@0 y@4 z@8
 *
 * Sum = outer.a(5) + inner.x(1) + inner.y(2) + inner.z(3) = 11.
 * RED-ON-DISABLE: a NAME-ONLY identity would MERGE the two `struct S` into one
 * (the file-scope 1-field layout), so the inner `.y`/`.z` would read past the
 * merged 4-byte type — a silent layout miscompile that derails the sum off 11. */
struct S { int a; };

int main(void) {
    struct S outer;
    outer.a = 5;
    {
        struct S { int x; int y; int z; } inner;
        inner.x = 1;
        inner.y = 2;
        inner.z = 3;
        return outer.a + inner.x + inner.y + inner.z;
    }
}
