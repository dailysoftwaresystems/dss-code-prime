// C 6.2.2p4: a BLOCK-scope `extern int g;` with a visible file-scope
// declaration of g denotes THE SAME object — it does not introduce a new one.
// gcc, clang and MSVC all build and run this file (probed 2026-09-01).
// Before P50, DSS refused this exact file with K_SymbolUndefined: the block
// extern lowered to an import row that the link tier would not satisfy from
// the same translation unit, while the SAME two declarations split across two
// files linked fine. The write through the block extern must reach the
// file-scope g, so the exit code proves the binding, not just the compile.
int g = 5;
void h(void) {
    extern int g;
    g = 42;
}
int main(void) {
    h();
    return g;
}
