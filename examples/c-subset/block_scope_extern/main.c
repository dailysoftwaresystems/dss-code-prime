// D-CSUBSET-BLOCK-SCOPE-EXTERN (TF arc C10, C89 6.7.1) witness: a function body
// that declares BOTH a block-scope extern FUNCTION prototype and a block-scope
// extern OBJECT reference, then uses both. Neither is defined in THIS TU; both
// are defined in def.c (the sibling TU), so the whole-program LK11 merge binds
// the block externs to def.c's definitions (the function import stripped + the
// call rewired DIRECT; the object reference reaching def.c's global) — the real
// SQLite test-TU shape (test_thread.c:116 `extern int Sqlitetest_mutex_Init(
// Tcl_Interp*);`, test_btree.c:32 `extern BtShared *sqlite3SharedCacheList;`).
//
// The block-scope extern binds into the BLOCK scope (C 6.2.2p4 name scope), NOT
// re-homed to file scope the way a bare prototype is — so a block extern that
// shadows an outer local reads the extern (design-audit Finding 3). collectExterns
// registers the symbol, so the block uses resolve via GlobalAddr, identical to a
// file-scope extern.
//
// Value-divergent: helper(g_base) = helper(40) = 40 + 2 = 42; a wrong binding
// (a null import slot, or a mis-scoped shadow) never produces 42.
//
// RED-ON-DISABLE (grammar): remove "externDecl" from the `statement` alt in
//   c-subset.lang.json -> P0009 "got 'extern'" at the first block-scope extern.
// RED-ON-DISABLE (lowering): remove the k=="ExternDecl" arm in cst_to_hir.cpp's
//   lowerStmt -> the externDecl statement hits the terminal default -> the
//   "statement maps to unsupported HIR kind 'ExternDecl'" fail-loud.

int main(void) {
    extern int helper(int);   // block-scope extern FUNCTION prototype (defined in def.c)
    extern int g_base;         // block-scope extern OBJECT reference   (defined in def.c)
    return helper(g_base);     // 40 + 2 = 42
}
