/* c28 RUNTIME WITNESS (D-CSUBSET-LOCAL-TYPE-DEFINITION): a BLOCK-SCOPED
 * struct DEFINITION with NO declarator, as a STATEMENT inside a function —
 * the sqlite3.c:68508 `walMergesort` shape (`struct Sublist { int nList;
 * ht_slot *aList; };` mid-function). The varDecl init-declarator-list is now
 * OPTIONAL (mirroring topLevelDecl), so the unified c25 structSpec DEFINES the
 * type in the ENCLOSING BLOCK scope; a later `struct Sublist v;` resolves it and
 * its fields drive the return value across a non-inlined call (so the fields
 * round-trip through a frame slot — a wrong offset / a leaked-or-merged type
 * derails the exit code).
 *
 *   struct Sublist { int nList; int aList; };   (DEFINITION — no declarator)
 *   build():  v.nList = 30, v.aList = 12  → sum 42
 *
 * RED-ON-DISABLE: revert the optional-list grammar tweak → `struct Sublist
 * { … };` is a parse error (P0009) and the corpus never builds. A scope-leak or
 * a name-only identity (a DIFFERENT-layout `struct Sublist` lives at file scope,
 * size 4 / one field) would read `aList` past the merged type → off 42.
 * Release pipeline + all 4 targets. */

/* A file-scope `struct Sublist` with a DIFFERENT (1-field) layout — the
 * block-scoped definition inside build() must stay DISTINCT from this (c24
 * decl-site identity), or `.aList` reads garbage. */
struct Sublist { int nList; };

int build(void) {
    /* The c28 construct: a bare block-scoped type DEFINITION (no declarator). */
    struct Sublist { int nList; int aList; };
    struct Sublist v;
    v.nList = 30;
    v.aList = 12;
    return v.nList + v.aList;
}

int main(void) {
    return build();
}
