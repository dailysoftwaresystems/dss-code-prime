/* P69 fold 3d (lane xa): the source of the checked-in `coff_weak_alias_of_strong_clangcl_O2.obj` -- a WEAK alias of a
 * STRONG definition, which clang-cl writes as a weak external (class 105) whose default is the definition's own record.
 * The pin reads it, links it beside another unit into DSS's relocatable artifact, and reads the artifact back: the
 * alias's default is named the same at each step.
 *
 * REBUILD (from tests/link/data; clang-cl stamps the object's TimeDateStamp, so a rebuild differs in those 4 bytes):
 *   clang-cl /nologo /c /O2 /GS- coff_weak_alias_of_strong.source.c /Focoff_weak_alias_of_strong_clangcl_O2.obj
 */
int strong_w = 7;
extern int weak_m __attribute__((weak, alias("strong_w")));
int reads_m(void) { return weak_m; }
