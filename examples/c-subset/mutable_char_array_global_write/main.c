/* D-CSUBSET-MUTABLE-CHAR-ARRAY-RODATA (folded into the c67 cycle, 2026-06-30 —
 * the user asked to fix it while we're editing asm.cpp's global-section code):
 * a MUTABLE file-scope `char m[N] = "str"` global is the array OBJECT itself (C
 * 6.7.9 — mutable storage), so it must live in writable `.data`, NOT read-only
 * `.rodata`. A runtime write (`m[0]='J'`) SEGFAULTs if it is wrongly in .rodata.
 * The asm string-literal arm used to force `.rodata` UNCONDITIONALLY; the fix
 * mints the SYNTHETIC string-literal-pool globals const (→ .rodata, correct) and
 * lets a NAMED mutable array honor its non-const declaration (→ .data). PRESERVE:
 * a `const` char array → .rodata, and `char *p="hi"` keeps its POINTED-TO bytes
 * in .rodata (writing THROUGH such a pointer is UB and may fault — correct).
 * RED-ON-DISABLE: restore the unconditional .rodata override → the `m[0]='J'`
 * write SEGFAULTs (exit 139). */

char        m[6] = "hello";       /* MUTABLE named array → writable .data        */
const char  c[4] = "abc";         /* const → read-only .rodata (correct)         */
char       *p    = "hi";          /* a mutable POINTER; its bytes stay .rodata   */

int main(void){
    m[0] = 'J';                   /* THE load-bearing write: faults if m is rodata */
    if (m[0] != 'J')               return 1;
    if (m[1] != 'e' || m[5] != 0)  return 2;   /* the rest of the init preserved   */
    if (c[0] != 'a' || c[3] != 0)  return 3;   /* const array still readable       */
    if (p[0] != 'h' || p[1] != 'i' || p[2] != 0) return 4;  /* pointer's rodata bytes readable */
    return 42;
}
