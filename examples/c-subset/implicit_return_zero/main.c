// Regression pin for D-LK10-ENTRY-MAIN-IMPLICIT-RETURN: C99
// §5.1.2.2.3 — `main` without an explicit return must end with
// an implicit `return 0`. The HIR lowering's
// `maybeAppendImplicitReturnZero` helper synthesizes the return
// before the function reaches the verifier; a regression that
// loses the synthesis surfaces here as the binary refusing to
// exit cleanly or trapping in main's epilogue.
int main() { }
