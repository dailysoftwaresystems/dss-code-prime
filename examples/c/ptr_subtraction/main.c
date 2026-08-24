/* c40 (D-CSUBSET-POINTER-SUBTRACTION) C 6.5.6p9 run-green witness: `p - q` (same
 * pointer type) yields a SIGNED ptrdiff_t whose VALUE is the ELEMENT count
 * (byte_difference / sizeof(*p)), NOT the byte count — and it passes as a numeric
 * function ARGUMENT (the sqlite `sqlite3StrAppend64(..., fmt - bufpt)` shape).
 *
 * RED-ON-DISABLE (two ways): pre-c40 `take(p4 - p1)` did NOT COMPILE — p-q was
 * typed Ptr<int> so passing it to a `long` param raised S_TypeMismatch; AND if it
 * had compiled, an int* difference with NO sizeof scaling would give 12 (bytes)
 * not 3 (elements). exit 42 requires: int* scaled (3, not 12), char* (5), and the
 * SIGNED negative (p1-p4 == -3). int ops + arrow/index only (no binary p+n).
 * 3*10 + 5 + 7 = 42. */
static int take(long n) { return (int)n; }   /* receives the ptrdiff as a numeric arg */

int main(void) {
    int  arr[8];
    int  *p4 = &arr[4];
    int  *p1 = &arr[1];
    char cbuf[10];
    char *c7 = &cbuf[7];
    char *c2 = &cbuf[2];
    int forward = take(p4 - p1);     /* int*: (16-4)/4 = 3 ELEMENTS (not 12 bytes) */
    int chars   = take(c7 - c2);     /* char*: 7 - 2 = 5 (stride 1) */
    int back    = take(p1 - p4);     /* SIGNED: (4-16)/4 = -3 */
    return forward * 10 + chars + (back == -3 ? 7 : 99);   /* 30 + 5 + 7 = 42 */
}
