/* c59 (D-CSUBSET-ARRAY-DECAY-IN-ADDITIVE): an ARRAY operand of `+`/`-` must decay to
 * Ptr<elem> (C 6.3.2.1p3) BEFORE the additive arm types the result. Without the decay
 * `array + i` kept TypeKind::Array -> the c41 ptrIntArith path did NOT fire -> the
 * BinaryOp mis-typed as the array -> a downstream `*(array + i)` Deref came out TYPELESS:
 *   - as an assignment LHS  ->  error[H0009] (lvalue-classify on a typeless target)
 *   - as an rvalue read     ->  error[H0001] (HirKind Deref unresolved, ordinal 39)
 *
 * SQLite construct (the AtomicStore/AtomicLoad macros over an array MEMBER, sqlite3.c:
 * 68868 / 69822):  `AtomicStore(pInfo->aReadMark + i, iMark)` == `*(aReadMark + i) = iMark`
 * and `AtomicLoad(pInfo->aReadMark + i)` == `*(aReadMark + i)`, where aReadMark is `u32
 * aReadMark[N]` (an array). The pointer forms `*(p+i)=v` and the subscript form `a[i]=v`
 * already compiled (proving it was array-decay-specific, not assignment-specific).
 *
 * RED-ON-DISABLE: revert the decay in combineBinary -> the `*(a + i) = ...` store fires
 * H0009 and the `*(a + i)` read fires H0001 (the file does not compile).
 *
 * VALUE-CORRECT + STRIDE-SENSITIVE: a runtime index `i` drives both forms. The element
 * type is `int` (stride sizeof=4) -> if the decayed pointer used a 1-byte stride (the
 * pre-c41 bug the decay reuses the fix for) the overlapping writes/reads would corrupt
 * the values and the result would NOT be 42. 3*1+3*2+3*3+3*4 = 30, + a[3]=12 -> 42. */
int main(void) {
    int a[4];
    int i;

    /* assignment-LHS through `*(array + i)`, RUNTIME i -- the AtomicStore(aReadMark+i,v)
     * form that fired H0009. a = {3, 6, 9, 12}. */
    for (i = 0; i < 4; i = i + 1) {
        *(a + i) = (i + 1) * 3;
    }

    /* rvalue read through `*(array + i)`, RUNTIME i -- the AtomicLoad(aReadMark+i) form
     * that fired H0001. 3 + 6 + 9 + 12 = 30. */
    int sum = 0;
    for (i = 0; i < 4; i = i + 1) {
        sum = sum + *(a + i);
    }

    /* `int + array` (canonicalized so the decayed Ptr is always kids[0]): a[3] = 12. */
    sum = sum + *(3 + a);

    return sum;   /* 30 + 12 = 42 */
}
