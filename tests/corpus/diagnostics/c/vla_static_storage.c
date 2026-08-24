// VLA C1a (D-CSUBSET-VLA, IMPORTANT-1): a block-scope variable-length array
// declared `static` has non-automatic storage duration — rejected loud by the
// Pass-2 validateVlaDeclarator (S_VlaWithStaticStorage), never lowered as a VLA.
int f(int n) {
    static int a[n];
    return 0;
}
