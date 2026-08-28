// OPT7 weak-inline negative pin (D-OPT7-WEAK-INLINE-NEGATIVE-PIN), CU A.
//
// `f` is WEAK here and `main` calls it INTRA-CU, so the Inlining pass ATTEMPTS
// to inline `f` into `main`. The §2.9 legality gate MUST REFUSE (a strong
// definition of `f` in a sibling CU may replace this weak one at link). So
// `main` keeps the out-of-line call; the linker then resolves the call to the
// STRONG `f` in cu_b.c (strong-over-weak) → the process exits 42.
//
// A BROKEN gate would inline this weak body (`return 7`) directly into main →
// the link-time strong-over-weak resolution is bypassed → exit 7. The corpus's
// differential ASSERT (optimized arm exit == baseline exit == 42) then fires.
__attribute__((weak)) int f() { return 7; }

int main() {
    return f();
}
