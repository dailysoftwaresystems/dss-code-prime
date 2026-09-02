// D-CSUBSET-LINKAGE-INTERNAL-EXTERNAL-MISMATCH (C 6.2.2p7): `int g;` gives g
// EXTERNAL linkage and defines it (a tentative definition); `static int g;`
// then claims INTERNAL linkage for the same identifier. This is the object
// ordering gcc, clang AND MSVC all reject (gcc/clang "static declaration of
// 'g' follows non-static declaration"; MSVC C2370 "redefinition; different
// storage class"), so DSS rejects it loud.
int g;
static int g;
int main(void) { return 42; }
