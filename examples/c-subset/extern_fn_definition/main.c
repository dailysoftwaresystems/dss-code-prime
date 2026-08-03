// D-CSUBSET-EXTERN-FN-DEFINITION witness (§B 2026-07-21, SQLite testfixture-link
// sub-arc): `extern` on a FUNCTION DEFINITION — `extern int f(int x){ … }`, valid
// C 6.9.1 and symmetric with `static int f(){ … }`. Before this cycle externDecl
// was declaration-only (no `{body}` arm) so this P0009'd ("expected
// StringStart/EndStatement … got '{'") — the SQLite testfixture MAIN harness
// `tclsqlite-ex.c` hit it ×14 at `EXTERN int Sqlite3_Init(Tcl_Interp*){ … }`
// (EXTERN = a tcl.h macro → `extern`).
//
// `addone` is an EXTERNAL-linkage definition (a real body is emitted — NOT an
// ExternFunction import); `main` calls it with a runtime arg. addone(41) = 42.
//
// RED-ON-DISABLE (grammar): revert externDeclTail's `block` arm -> `extern int
// addone(int x){ … }` P0009s at the `{`. RED-ON-DISABLE (HIR): if the definition
// were mis-lowered as an extern DECLARATION (the pre-cycle ExternFunction path),
// `addone` would be an unresolved import -> link failure, never exit 42.

// NOTE: main takes a RUNTIME arg (argc) so the `release` optimizedPipeline arm
// genuinely exercises the extern function at runtime instead of const-folding the
// call. Spawned with no args -> argc==1 -> addone(1 + 40) = addone(41) = 42.
extern int addone(int x) { return x + 1; }

int main(int argc, char** argv) {
    (void)argv;
    return addone(argc + 40);   // argc==1 -> addone(41) -> 42
}
