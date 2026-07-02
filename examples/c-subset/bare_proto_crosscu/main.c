// c86 (D-CSUBSET-BARE-PROTO-EXTERN-SYNTHESIS) witness: a BARE function
// prototype (`int addRemote(int);` — NO `extern` keyword, the sqlite3.h splice
// shape where SQLITE_API expands empty) with no in-TU definition synthesizes a
// NO-LIBRARY extern reference (C 6.2.2p5: external linkage refers to a
// definition somewhere in the PROGRAM). The whole-program LK11 merge binds it
// to def.c's definition — the import row is stripped and the call is rewired
// to a DIRECT intra-module call (no import-table entry for `addRemote`).
//
// Value-divergent: addRemote(30) = 30 + 12 = 42; a wrong binding (H0009
// pre-c86, or a null import slot) never produces 42.
//
// RED-ON-DISABLE: drop `prototypeSynthesizesExtern` from the topLevelDecl row
// in c-subset.lang.json -> the proto emits nothing again and this program
// FAILS to compile with H0009 (HIR Ref to unbound symbol) at the call.

int addRemote(int base);   // bare prototype — defined in def.c, the sibling TU

int main(void) {
    return addRemote(30);  // 42
}
