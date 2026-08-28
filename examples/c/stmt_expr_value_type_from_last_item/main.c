// D-C-SUBTREE-TYPE-WALKS-INTO-A-STATEMENT-EXPRESSION-BODY (P32 lane A) witness:
// a GNU statement expression whose BODY DECLARES something of a different type
// than the body YIELDS.
//
// The semantic tier's expression typer (`subtreeType`) walked INTO the body,
// met a declaration, and took the DECLARATOR's type — so the construct was
// judged as the wrong operand. ✔MEASURED through the shipped CLI at the
// pre-change HEAD, bisected so the trigger is exact, not guessed:
//
//     *({ *q; })                      ✅   (no declaration in the body)
//     *({ int *r = p; r; })           ✅   (declarator type == yielded type)
//     int x = *({ int **q = &p; *q; });  ✗  false `S0003`
//
// `q` is `int **`, the body yields `int *`, and the dereference was checked
// against `q`'s type. It was a REFUSAL of valid C, never a miscompile — which is
// exactly why it needs a runnable witness: the failure mode is "correct code
// does not build", and nothing but building it proves the refusal is gone.
//
// ★ THE FIX TAKES THE BODY'S TYPE FROM THE SAME PLACE THE LOWERING TAKES ITS
// VALUE — the last item's expression, recognised by HIR kind `ExprStmt` exactly
// as `stmtExprItems` does — so the two tiers cannot disagree about what a body
// yields. The `void` case (a body whose last item is NOT an expression statement)
// stays a REFUSAL in value position, matching gcc and clang, and is pinned in
// `tests/analysis/semantic/test_stmt_expr_value_type.cpp` because a corpus
// example cannot carry a program that must fail to build.
//
// ★ VERIFIED against BOTH references, `-std=c2x -Wall -Wextra`: gcc 13.3.0 and
// clang 19.1.7 compile this file with zero errors and zero warnings, and both
// binaries exit 42.
//
// RED-ON-DISABLE: remove the statement-expression arm from `subtreeType` in
// `src/analysis/semantic/semantic_analyzer.cpp` (the one keyed on
// `hirLowering.stmtExprRule`) → the first initializer fails S0003 and the program
// no longer compiles.
//
// Front-end feature (semantic expression typing), target/format-agnostic:
// x86_64 (PE + ELF) and arm64 (ELF under qemu, Mach-O macos leg).

int main(void) {
    int  v  = 20;
    int *p  = &v;
    // The exact repro: the body declares `int **` and yields `int *`.
    int a = *({ int **q = &p; *q; });          // 20

    int  w  = 12;
    int *pw = &w;
    // The control that already worked — declarator type == yielded type.
    int b = *({ int *r = pw; r; });            // 12

    // A body that declares a type sharing nothing with what it yields, so a walk
    // that took the first declarator would come back with `double`.
    int c = ({ double d = 0.5; int t = 10; (void)d; t; });   // 10

    return a + b + c;                          // 42
}
