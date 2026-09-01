// D-CSUBSET-LINKAGE-INTERNAL-EXTERNAL-MISMATCH (C 6.2.2p7): the FUNCTION
// flavor — a plain definition gives f external linkage; the later `static`
// declaration claims internal. gcc/clang "static declaration of 'f' follows
// non-static declaration"; MSVC C2375 "redefinition; different linkage".
// (The REVERSE ordering — static proto, then a plain definition — is legal
// C 6.2.2p4/p5 inheritance and stays accepted: see linkage_static_extern_legal.)
int f(void) { return 42; }
static int f(void);
int main(void) { return f(); }
