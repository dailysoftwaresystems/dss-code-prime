/* D-CSUBSET-EXTERN-INCOMPLETE-ARRAY witness (C 6.7p7 / 6.9.2): an extern
** OBJECT declaration with an INCOMPLETE array type — the exact sqlite3.h
** shape `SQLITE_API SQLITE_EXTERN const char sqlite3_version[];` that
** shell.c compiles against while sqlite3.c owns the definition. This TU
** declares `tbl[]` incomplete and reads through it (array decay + indexing
** need only the ELEMENT type); data.c DEFINES it sized. The MIR merge (LK11)
** resolves the extern-data import onto the sibling CU's global — the import
** row is stripped and the references land on the real storage.
**
** tbl = {40, 2} -> tbl[0] + tbl[1] = 42.
**
** RED-ON-DISABLE: drop `allowFlexibleArray` from the c-subset `externDecl`
** row -> `extern const char tbl[];` errors S000B (S_NonConstantArrayLength)
** — the program no longer COMPILES. gcc -std=c17 cross-checked (two-TU
** compile), exits 42.
**
** Front-end + merge feature; target- and format-agnostic (pe + elf legs).
*/

extern const char tbl[];

int main(void) {
  int i;
  int s = 0;
  for (i = 0; i < 2; i = i + 1) {
    s = s + tbl[i];
  }
  return s;   /* 40 + 2 = 42 */
}
