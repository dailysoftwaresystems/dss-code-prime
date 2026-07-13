// VLA C1a (D-CSUBSET-VLA, MINOR-2): a multi-dimensional VLA `int a[n][m]` needs a
// runtime STRIDE through index/GEP — the C3 boundary. The inner `[m]` folds to a
// vlaArray; the outer `[n]` VLA arm rejects a VLA/array element loud
// (S_VlaMultiDimUnsupported), never a silent nested-VLA.
int f(int n, int m) {
    int a[n][m];
    return 0;
}
