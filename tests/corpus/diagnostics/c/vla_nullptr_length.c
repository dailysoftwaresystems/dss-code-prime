// VLA C1a (D-CSUBSET-VLA, C11 §6.7.6.2p1): a variable-length array whose SIZE is
// `nullptr` (nullptr_t, NOT an integer) is a constraint violation. It MUST be caught
// at the SEMANTIC tier (S_VlaSizeNotInteger) — `nullptr` lowers to an I32 0 by MIR,
// so no downstream tier can distinguish it from a legal integer 0 (a silent 0-byte
// array). This is the case a MIR-tier integer check cannot catch.
int f(void) {
    int a[nullptr];
    return 0;
}
