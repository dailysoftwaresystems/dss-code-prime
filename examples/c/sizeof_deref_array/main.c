/* D-CSUBSET-SIZEOF-DEREF-ARRAY-SILENT-FALLBACK: `sizeof(*arr)` where the operand
 * DEREFERENCES an array must size the ELEMENT, not the whole array. C 6.3.2.1p3:
 * dereferencing an array first decays it to Ptr<elem>, so `*arrayOfT` has type
 * `T` and `sizeof(*arrayOfT) == sizeof(T)` (C 6.5.3.4). It is EXACTLY `arr[0]`.
 *
 * THE BUG (silent miscompile, all 4 targets): the shared `derefResultType` law
 * had NO array arm, so the semantic tier left `*arr` UNSTAMPED and lowerSizeof's
 * `resolveStampedTypeBelow` DFS-descended past the unstamped deref into the LEAF
 * `arr` — sizing the WHOLE array. Pre-fix silent values (each row's failure code):
 *   sizeof(*a)      -> 40  (int[10], want 4)          [row 1]
 *   sizeof(*m)      -> 48  (int[3][4], want 16)        [row 2]
 *   sizeof(*"hello")-> 6   (char[6],  want 1)          [row 3]
 *   sizeof(*da)     -> 40  (double[5], want 8)         [row 6]
 *   sizeof(*ua)     -> 7   (uchar[7], want 1)          [row 7]
 * FIX: (a) `derefResultType(Array<T,N>) = T` in the shared law (the sibling of
 * `indexResultType`, which already decays an Array base for `[]`), so the operand
 * gets stamped with the element type; (b) lowerSizeof's VALUE form reads the
 * operand's DIRECT stamp and FAILS LOUD if it is missing — no silent DFS guess.
 *
 * RED-ON-DISABLE: revert the `derefResultType` Array arm and rows 1/2/3/6/7 lose
 * their element stamp -> WITH the lowerSizeof wall they FAIL LOUD (the operand is
 * untyped -> a compile error, never a wrong binary); ALSO revert the wall and the
 * old DFS silently returns 40/48/6/40/7 -> a non-42 exit. Sizes are target-
 * INDEPENDENT (int 4, int[4] 16, char 1, double 8 on every data model — NO `long`,
 * which is 4 on LLP64/pe64). Rows 4/5/8..11 are CONTROLS that were already correct
 * (Ptr deref + whole-array + index) — they pin that the fix touched only the
 * array-DEREF type, nothing else. Each wrong size returns its own distinct code. */

int main(void) {
    int  a[10];              /* sizeof(*a)  = int      = 4  (NOT the 40-byte array) */
    int  m[3][4];            /* sizeof(*m)  = int[4]   = 16 (the ROW, not the 48)   */
    int (*pm)[4] = m;        /* pointer to int[4]: m decays; sizeof(*pm) = int[4]   */
    int  *pa     = a;        /* plain pointer control: sizeof(*pa) = int = 4        */
    double        da[5];     /* sizeof(*da) = double   = 8  (8 on every target)     */
    unsigned char ua[7];     /* sizeof(*ua) = uchar    = 1                          */

    /* --- the array-decay-then-deref rows (WRONG pre-fix) --- */
    if (sizeof(*a) != 4)          return 1;   /* was 40: whole int[10]  */
    if (sizeof(*m) != 16)         return 2;   /* was 48: whole int[3][4] */
    if (sizeof(*"hello") != 1)    return 3;   /* was 6:  char[6] literal */
    if (sizeof(*da) != 8)         return 6;   /* was 40: whole double[5] */
    if (sizeof(*ua) != 1)         return 7;   /* was 7:  whole uchar[7]  */

    /* --- controls: already correct, must stay correct --- */
    if (sizeof(*pm) != 16)        return 4;   /* Ptr<int[4]> deref -> int[4] */
    if (sizeof(*pa) != 4)         return 5;   /* Ptr<int>   deref -> int     */
    if (sizeof(a) != 40)          return 8;   /* whole int[10] unchanged     */
    if (sizeof(m) != 48)          return 9;   /* whole int[3][4] unchanged   */
    if (sizeof(a[0]) != 4)        return 10;  /* index element == deref elem */
    if (sizeof(m[0]) != 16)       return 11;  /* index row     == deref row  */

    return 42;
}
