// D-CSUBSET-NEG-INT-TO-PTR-SIGN-EXTEND: a signed narrower `int` converted to a
// pointer must SIGN-extend to pointer width (the universal C convention real code
// relies on — e.g. sqlite's `(const u8*)(-1)` max-pointer sentinel). A volatile-
// seeded RUNTIME `int -1` -> `void*` must produce an all-ones (sign-extended)
// pointer, so observing the HIGH 32 bits distinguishes the fix from the old
// zero-extend bug (which produced 0x0000_0000_FFFF_FFFF).
//
// The observation type is `unsigned long long` (64-bit on EVERY target, incl.
// Windows LLP64 where `unsigned long` is only 32-bit and would TRUNCATE the very
// bits under test -> a vacuous witness). Same answer (20) on every target:
// sign-extension is target-independent; only sqlite's arm64 address layout (>4 GiB)
// turned this value bug into a runtime failure. The `release` arm proves the
// optimizer's ConstFold does not mis-fold the extension (it folds neither SExt/ZExt
// nor IntToPtr). RED-ON-DISABLE: revert the combineCast widen -> the pointer
// zero-extends -> (v >> 32) == 0 -> exit 10.
int main(void) {
    volatile int seed = -1;                 // runtime source: defeats const-fold
    void* p = (void*)seed;                    // int -1 -> void* : the conversion under test
    unsigned long long v = (unsigned long long)p;
    return (v >> 32) != 0ULL ? 20 : 10;       // 20 = sign-extended (correct); 10 = zero-extend BUG
}
