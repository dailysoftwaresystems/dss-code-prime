// FC4 c1 stage 2b — the volatile WALL (D-CSUBSET-VOLATILE-SEMANTICS):
// `volatile` is grammar-ADMITTED (headQualifier/ptrQualifier) but its
// semantics (no caching / no reordering of accesses) are NOT implemented
// by the optimizer tiers, so every use fails LOUD via the config-driven
// `gatedMarkers` row (token VolatileKeyword -> S_VolatileNotSupported)
// instead of silently compiling v as plain memory and miscompiling
// device/MMIO-style code under LICM/CSE/DCE.
//
// The diagnostic lands ON the `volatile` token itself: line 11, col 5.
int main() {
    volatile int v = 1;
    return v;
}
