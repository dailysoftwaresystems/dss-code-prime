/* c-TF (D-CSUBSET-ARRAY-DECAY-IN-DEREF): unary `*` applied to an ARRAY operand must
 * decay the array to Ptr<elem> (C 6.3.2.1p3 — the SAME law as c59's additive decay)
 * BEFORE the Deref types its result. Without the decay `*(arrayName)` kept
 * TypeKind::Array -> derefResultType (Ptr-only, the shared semantic law) returned
 * InvalidType -> the Deref came out TYPELESS -> error[H0001] (HirKind Deref
 * unresolved, ordinal 43). `arrayName[0]` (Index -> indexResultType types an Array
 * base) and `*(arrayName + 0)` (c59 additive-decay) already compiled -> proving this
 * was the DIRECT-deref-of-an-array hole, independent of any comma / ternary.
 *
 * SQLite construct (sqliteInt.h getVarint32 / putVarint32; the test3.c:474 blocker):
 *   getVarint32(A,B) == (u8)((*(A)<0x80) ? ((B)=(u32)*(A)),1 : slow)    -- RVALUE  *(A)
 *   putVarint32(A,B) == (u8)(((u32)(B)<0x80) ? (*(A)=(u8)(B)),1 : slow) -- LVALUE  *(A)
 * where A is `unsigned char zBuf[100]` (an ARRAY) dereferenced DIRECTLY inside the
 * comma-in-the-middle-of-a-ternary. `getVarint32(zBuf,out32)` fired two H0001.
 *
 * RED-ON-DISABLE: revert the Deref array-decay in cst_to_hir `combineUnaryOp` -> both
 * `*(zBuf)` (the getVarint32 rvalue read) and `*(wBuf)` (the putVarint32 lvalue store)
 * come out typeless and the file fails to compile (H0001). The exit value is `out`,
 * assigned SOLELY by the getVarint32 comma-LEFT side effect through the array deref, so
 * a dropped side effect or a mis-read/mis-strided deref would NOT yield 42.
 *
 * VALUE-CORRECT + RUNTIME-DRIVEN: the bytes are seeded through a runtime loop (the
 * already-working index form `a[i]=`), so the directly-deref'd byte is not a lone
 * front-end constant. Pure c-subset -> all 4 targets; pe RUNS on Windows, elf/arm64
 * RUN on linux/qemu. */
typedef unsigned char u8;
typedef unsigned int  u32;

int main(void) {
    unsigned char zBuf[4];   /* the sqlite `unsigned char zBuf[100]` array */
    unsigned char wBuf[4];
    u32 out = 0;
    int i;

    /* runtime seed via the index form: zBuf = {42, 41, 40, 39}; wBuf = {0,0,0,0}. */
    for (i = 0; i < 4; i = i + 1) { zBuf[i] = (u8)(42 - i); wBuf[i] = 0; }

    /* getVarint32(zBuf,out) EXPANDED: the RVALUE array-deref `(u32)*(zBuf)` inside the
     * comma-LEFT of the ternary middle assigns `out`; the comma yields 1. out = 42. */
    u8 n1 = (u8)((*(zBuf) < (u8)0x80) ? ((out) = (u32)*(zBuf)), 1 : 9);

    /* putVarint32(wBuf,out) EXPANDED: the LVALUE array-deref `*(wBuf) = (u8)out` inside
     * the ternary middle stores 42 into wBuf[0]; the comma yields 1. */
    u8 n2 = (u8)(((u32)(out) < (u32)0x80) ? (*(wBuf) = (unsigned char)(out)), 1 : 9);

    if (n1 != n2)                      return 1;   /* both fast paths -> 1 == 1 */
    if (wBuf[0] != (unsigned char)out) return 2;   /* the lvalue store landed */
    return (int)out;                               /* 42 */
}
