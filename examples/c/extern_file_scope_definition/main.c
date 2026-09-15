// the FILE-SCOPE half of [[D-FF2-3]] (P65) — THE USING
// translation unit. It declares each of def.c's objects the ordinary way and
// reads them back, so the example gates on the LINK and on the VALUES, not on
// rc 0 from the front end.
//
// WHAT WOULD GO WRONG WITHOUT THE FIX, in order of how quietly it fails:
//   * the front end refuses def.c outright (the pre-P65 behaviour);
//   * def.c is accepted but lowered as an IMPORT rather than a definition —
//     nothing defines these symbols anywhere in the program and the link fails
//     loud with an undefined symbol;
//   * def.c defines `g_other` as well as `g_pair` (a declaration-level answer
//     to a per-declarator question) — a DUPLICATE definition against this
//     file's `int g_other = 3;`, which the linker refuses.
// Only the correct reading links AND returns 42.
//
// exit = 23 + 7 + (1+2+2) + 4 + 3 = 42. Data-model independent: every value is
// a small int.
//
// RED-ON-DISABLE (REMOVE direction, ENGINE mutant — source AND object md5 both
// move and return): in src/hir/lowering/cst_to_hir.cpp's `lowerExternDeclInto`,
// delete the `rec->isExternDeclaration` split so every initialized declarator
// goes back to emitting H_ExternHasInitializer. def.c then fails to compile and
// the example never links.

extern int g_defined;
extern const int g_const;
extern int g_arr[3];
extern int g_pair;

// The other half of def.c's per-declarator arm: `g_other` was DECLARED there,
// never defined, so this is the program's only definition of it.
int g_other = 3;

int main(void) {
    int sum = g_defined + g_const;
    sum = sum + g_arr[0] + g_arr[1] + g_arr[2];
    sum = sum + g_pair + g_other;
    return sum;
}
