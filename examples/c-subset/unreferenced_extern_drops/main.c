// D-LINK-EXTERN-IMPORT-REFERENCE-GATE (TF-C44, SQLite testfixture-link sub-arc):
// RUN witness that an UNREFERENCED non-eager `extern` FUNCTION declaration emits
// NO dynamic import — gcc's rule that an unused declaration produces no reference.
//
// `never_referenced_fn` is DECLARED but NEVER called. Before the reference-gate
// generalization, DSS's explicit-`extern` producer (cst_to_hir.cpp `recordExtern`)
// 3-field-inited HirExternRecord with `noLibraryBinding=false`, so the FFI synth
// AUTO-BOUND the symbol to the ELF format-default library (libc.so.6). That
// library-bound row BYPASSED the linker's reference gate (which only touched
// zero-library rows) and flowed to the ELF dynamic walker as a real dynamic
// import — so ld.so failed at LOAD time with `undefined symbol:
// never_referenced_fn` and the process never started (exit 127). gcc emits no
// reference for an unused declaration; the loader has nothing to resolve.
//
// After the fix, the linker's `rejectOrDropUnreferencedExterns` DROPS an
// unreferenced NON-EAGER import whether or not a library bound it (an EAGER
// `#include`d descriptor import is still kept — D-FFI-DESCRIPTOR-EAGER-IMPORT is
// untouched). With no reference and no eager flag, `never_referenced_fn` is
// dropped, the binary carries no spurious import, ld.so loads it cleanly, and
// `main` returns 42.
//
// This is exactly the shape of the SQLite testfixture's `Sqlitetestsse_Init`
// (declared in src/test_tclsh.c, never called) — the sole load-blocker for the
// 88-TU testfixture link.
//
// RED-ON-DISABLE: revert the gate to only touch zero-library rows (or drop the
// `!isEagerImport` term so bound rows are never erased) → `never_referenced_fn`
// binds to libc.so.6, survives to the walker, and this ELF binary fails to LOAD
// (exit 127, `undefined symbol: never_referenced_fn`) instead of exiting 42.

extern int never_referenced_fn(int);

int main(void) {
    return 42;
}
