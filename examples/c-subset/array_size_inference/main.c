/* c34 (D-CSUBSET-ARRAY-SIZE-INFERENCE, C 6.7.9p22): an array declared with an
 * EMPTY bound `[]` and an INITIALIZER infers its size from that initializer —
 *   char s[] = "..."   →  Array<char, decoded-length + 1>  (the trailing NUL)
 *   int  a[] = {...}    →  Array<int, top-level-element-count>
 * The size is completed ONCE in the semantic model (Pass 1.5), so layout,
 * element access, and both HIR paths agree on N.
 *
 * The frontier is sqlite3.c:506  `const char sqlite3_version[] = SQLITE_VERSION;`
 * (i.e. `const char x[] = "3.x.y";`), mirrored by `ver` below.
 *
 *   s   = "DSS"            inferred char[4]  ('D' 'S' 'S' '\0')
 *   a   = {2, 7, 11, 22}   inferred int[4]
 *   ver = "9"              inferred const char[2]  ('9' '\0')
 *
 *   i   = pick(3)          a RUNTIME index, opaque across the un-inlined pick()
 *   r   = a[i]             a[3] == 22         (storage-array index, runtime i)
 *   r  += s[0]             'D' == 68          (inferred string element)
 *   r  -= ver[0]           '9' == 57          (inferred const-string element)
 *   return r - 11          22 + 68 - 57 - 11  ==  22
 * Wait — recomputed to land on 42 exactly below.
 *
 *   22 + 68 - 57 = 33;  +9 = 42  →  add ver[0]-'0' (== 9), not subtract.
 *   return a[i] + s[0] - 57 + (ver[0] - 48)
 *        = 22   + 68   - 57 + 9              = 42
 *
 * RED-ON-DISABLE: without size inference the `[]` declarations fail to compile
 * at all (S_NonConstantArrayLength); with a WRONG inferred size the element
 * reads (a[3], ver[0]) land off the intended storage and the exit is not 42.
 * The release / optimized arm keeps `i` a runtime value across pick() so the
 * element-access path is exercised (no whole-expression const-fold). */

int pick(int x) { return x; }

char s[]   = "DSS";
int  a[]   = {2, 7, 11, 22};
const char ver[] = "9";

int main(void) {
    int i = pick(3);
    return a[i] + s[0] - 57 + (ver[0] - 48);   /* 22 + 68 - 57 + 9 == 42 */
}
