// VLA C1a (D-CSUBSET-VLA, C11 §6.7.6.2p1): a variable-length array whose SIZE has
// FLOATING type (`int a[1.5]`) is a constraint violation — the size must have
// integer type. Rejected loud at the SEMANTIC tier (S_VlaSizeNotInteger), never a
// silently FPToSI-truncated element count at codegen.
int f(void) {
    int a[1.5];
    return 0;
}
