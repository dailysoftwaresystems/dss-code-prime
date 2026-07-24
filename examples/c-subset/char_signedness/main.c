// TF-C56 (D-CSUBSET-BARE-CHAR-SIGNEDNESS-PER-TARGET): bare `char`'s signedness
// is TARGET-defined. The AArch64 procedure-call standard makes bare `char`
// UNSIGNED; x86_64 (SysV + Windows/pe64) makes it SIGNED. A `char` holding the
// byte 0x80 therefore promotes to `int` as -128 on x86_64 (SExt / sxtb) but as
// +128 on arm64 (ZExt / uxtb), so `c < 0` is TRUE on x86_64 and FALSE on arm64.
//
// The `volatile` seed keeps the byte a RUNTIME value, so the optimizer cannot
// fold `(char)0x80 < 0` at compile time — the release arm exercises the real
// machine char->int promotion (the exact codegen this cycle fixes), not a
// const-fold. Per-target exitCode (see expected.json): 10 on the signed-char
// targets (x86_64 elf/pe64), 20 on the unsigned-char targets (arm64 elf/macho).

int main(void) {
    volatile int seed = 0x80;   // runtime source — not const-foldable
    char c = (char)seed;        // truncate to a bare `char` holding the byte 0x80
    return c < 0 ? 10 : 20;     // char->int promotion: SExt (signed) vs ZExt (unsigned)
}
