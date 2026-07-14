// VLA C3 (D-CSUBSET-VLA, IMPORTANT-7): a MULTI-DIMENSIONAL VLA now RUNS (`int a[n][m]`
// lowers to a runtime row stride) — but EVERY dimension's bound must still have integer
// type (C 6.7.6.2p1). The per-dim validator enumerates ALL suffixes, so a NON-integer
// INNER dim (`[m]`, where `m` is float) is rejected ON THAT DIM (S_VlaSizeNotInteger),
// never silently accepted. Red-on-regression for the multi-dim per-dim check.
int f(int n, float m) {
    int a[n][m];
    return 0;
}
