// D-CSUBSET-EXTERN-MULTI-DECLARATOR witness (C 6.7): a MULTI-declarator `extern`
// declaration — one `extern` head binding N declarators, each with its OWN
// pointer/array suffix — at BOTH file scope AND block scope. Before this cycle the
// legacy single-declarator `externDecl` P0009'd on the comma (`extern int a, b;`
// — sqlite test1.c:9341 `extern int sqlite3_sync_count, sqlite3_fullsync_count;`).
//
// All externs are defined in the sibling TU def.c; the whole-program LK11 merge
// binds each block/file extern to def.c's definition (object loads reach the
// globals) — the real cross-TU shape. Value-divergent: a wrong per-declarator TYPE
// (a shared-head star mis-typing `farr` as a pointer, or `fp` as a plain int) or a
// mis-scoped block extern never produces 42.
//
//   fa=5 fb=7  *fp=3  farr[0]=4 farr[1]=6  bc=8 bd=9  ->  5+7+3+4+6+8+9 = 42
//
// RED-ON-DISABLE (grammar): revert externDecl to the single-declarator spine ->
//   P0009 "got ','" at the first comma. RED-ON-DISABLE (per-declarator type): a
//   shared-head pointer would type `farr` as int* -> `farr[0]+farr[1]` reads
//   garbage, never 42.

extern int fa, fb;           // file-scope: TWO objects, ONE `extern` declaration
extern int *fp, farr[2];     // file-scope: per-declarator pointer (fp:int*) + array (farr:int[2])

int main(void) {
    extern int bc, bd;       // block-scope: TWO objects, ONE `extern` declaration
    return fa + fb + *fp + farr[0] + farr[1] + bc + bd;   // 42
}
