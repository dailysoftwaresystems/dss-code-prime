// C 6.2.2p4/p5 linkage inheritance — every ordering in this file is accepted
// by gcc, clang AND MSVC (probed separately, 2026-09-01): after a visible
// `static` declaration, a later `extern` declaration and a later PLAIN
// function declaration both INHERIT the internal linkage rather than
// contradicting it, and a further `static` re-declaration of the now-internal
// identifier is an ordinary legal repeat. DSS carries the internal-linkage
// bit across each redeclaration merge, which is exactly what keeps
// S_LinkageRedeclarationMismatch from mis-firing on any line here.
static int g;
extern int g;
static int f(void);
int f(void) { return 42 + g; }
static int f(void);
int main(void) { return f(); }
