/* c38 (D-CSUBSET-NESTED-TAG-SCOPE) runtime witness: a struct tag `SegInfo`
 * defined NESTED inside `struct WalSeg` is referenced BY NAME (`struct SegInfo
 * *`) in a separate function — the sqlite WalIterator/WalSegment + sColMap +
 * IdList_item shape. C 6.2.1p4: a tag declared in a struct body has the
 * ENCLOSING (file) scope, not the inner struct's member scope. The pointer
 * `&w.info` (the member) and the by-name `struct SegInfo *` must be the SAME
 * nominal type (c24 decl-site identity), or the field reads derail.
 *
 * RED-ON-DISABLE: pre-c38 `struct SegInfo` was trapped in WalSeg's member scope
 * → a by-name `struct SegInfo *` was unknown/incomplete → `s->iNext` raised
 * S_NotAComposite → this corpus would NOT EVEN COMPILE. int fields + arrow
 * access only (no indexing → no binary p+n). 3 + 4 + 0 = 7. */
struct WalSeg { struct SegInfo { int iNext; int nEntry; } info; int total; };

static int readSeg(struct SegInfo *s) {
    return s->iNext + s->nEntry;      /* by-name nested-tag pointer: member reads */
}

int main(void) {
    struct WalSeg w;
    w.info.iNext = 3;
    w.info.nEntry = 4;
    w.total = 0;
    struct SegInfo *p = &w.info;       /* &member typed as the by-name nested tag */
    return readSeg(p) + w.total;       /* 3 + 4 + 0 = 7 */
}
