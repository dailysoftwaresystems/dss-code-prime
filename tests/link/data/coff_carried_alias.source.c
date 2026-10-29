/* P69 fold 3d (lane xa): the source of the checked-in objects `coff_carried_alias_{a,b}_{clangcl,gcc}_O2.obj` --
 * lane cs's probe wk1, cell 3. Unit a holds a select-any copy of `w` (7) and reads it; unit b holds another (9), a
 * WEAK alias `m` of it, and reads `m`. clang-cl writes `m` as a weak external whose default is `w`, the COMDAT's own
 * symbol; MinGW gcc writes it as a weak external whose default is a label of its own inside `w`'s COMDAT section.
 *
 * REBUILD (from tests/link/data; clang-cl stamps each object's TimeDateStamp, so a rebuild differs in those 4 bytes):
 *   clang-cl /nologo /c /O2 /GS- coff_carried_alias.source.c /Focoff_carried_alias_a_clangcl_O2.obj
 *   clang-cl /nologo /c /O2 /GS- /DUNIT_B coff_carried_alias.source.c /Focoff_carried_alias_b_clangcl_O2.obj
 *   gcc -std=gnu17 -O2 -c -x c coff_carried_alias.source.c -o coff_carried_alias_a_gcc_O2.obj
 *   gcc -std=gnu17 -O2 -DUNIT_B -c -x c coff_carried_alias.source.c -o coff_carried_alias_b_gcc_O2.obj
 */
#ifndef UNIT_B
__declspec(selectany) int w = 7;
int ua(void) { return w; }
#else
__declspec(selectany) int w = 9;
extern int m __attribute__((weak, alias("w")));
int ub(void) { return m; }
#endif
