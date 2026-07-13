// VLA C1a (D-CSUBSET-VLA, IMPORTANT-2): a CONSTANT outer bound over a VLA element
// `int a[5][n]` — the inner `[n]` folds to a vlaArray, then the constant `[5]`
// folds over it. The FAM guard checks the -1 sentinel and would miss the -2 VLA
// element, so the constant arm has its own isVlaArray reject
// (S_VlaMultiDimUnsupported) — never a silent array(vlaArray).
int f(int n) {
    int a[5][n];
    return 0;
}
