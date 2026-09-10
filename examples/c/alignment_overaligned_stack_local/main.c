// P63 [[D-CSUBSET-ALIGNMENT-CEILING-REFUSES-WHAT-TWO-REFERENCES-RUN]] — the
// AUTOMATIC-STORAGE arm of the raised alignment ceiling, which had NO witness
// anywhere in the tree and is exactly why a FOURTH hand-written copy of the old
// 256 cap survived the cycle that removed the other three.
//
// The sibling example `alignment_page_request` covers the TYPE and the
// STATICALLY ALLOCATED object. Neither reaches the stack: a local's alignment
// travels on the MIR Alloca's secondary payload and is checked by the MIR
// verifier, a tier no `struct`/`static` subject enters. With the literal still
// in `src/mir/mir_verifier.cpp`'s Alloca arm, `_Alignas(4096) char buf[4096];`
// inside a function died on
//   error[I_AllocaAlignmentNotPowerOfTwo]: ... mir inst #1: alloca alignment
//   payload 4096 is not a power of two in [1, 256]
// — an INTERNAL `I_*` code naming a MIR instruction index, with NO source
// position line at all, where the base commit had given a POSITIONED,
// user-facing `error[S_AlignasExceedsMax]`. So the cycle that "raised the
// ceiling" made this case strictly WORSE for the user: same refusal, worse
// diagnostic.
//
// ✔MEASURED 2026-09-07, each reference probed SEPARATELY, BUILD **and** RUN:
// mingw-w64 gcc 13.2.0's AUTOMATIC arms are BUILD 0 / RUN 42 at 4096, 8192,
// 16384 and 65536 — the PE object-file ceiling that binds a STATIC (8192) does
// NOT bind a stack local on the very same compiler — and gcc 13.3.0 and clang
// 18.1.3 run locals green through 2^20. Three references build and run what DSS
// refused.
//
// ★ WHY THIS RUNS RATHER THAN MERELY COMPILING. The failure mode of the FIX is
// the silent one: an alignment accepted and then not applied. So every arm
// derives its result from the ADDRESS THE FRAME LAYOUT ACTUALLY CHOSE, never
// from a constant. `&buf[0] % N` is computed at run time; a slot placed on the
// wrong boundary returns 50+k naming which arm failed.
//
// ★ BOTH SPELLINGS, because the row's scope note says the two surfaces must
// move together or one construct means two things by spelling.

// Kept modest so the frame stays sane on all four target legs; the mechanism is
// identical at every value, and 512 is already past the old cap.
#define A_SMALL   16     // CONTROL — always worked
#define A_OLD_CAP 256    // CONTROL — the old boundary, the last value that worked
#define A_OVER    512    // the first value the old cap refused
#define A_PAGE    4096   // a page: the first alignment a real program writes

// Returns 0 on success, or the arm number on failure — kept out of `main` so
// the locals are genuine frame slots rather than anything a whole-program pass
// could fold away.
static int check(int seed) {
    _Alignas(A_SMALL)   char small_std[A_SMALL];
    _Alignas(A_OLD_CAP) char cap_std[A_OLD_CAP];
    _Alignas(A_OVER)    char over_std[A_OVER];
    _Alignas(A_PAGE)    char page_std[A_PAGE];

    char small_gnu[A_SMALL]   __attribute__((aligned(A_SMALL)));
    char cap_gnu[A_OLD_CAP]   __attribute__((aligned(A_OLD_CAP)));
    char over_gnu[A_OVER]     __attribute__((aligned(A_OVER)));
    char page_gnu[A_PAGE]     __attribute__((aligned(A_PAGE)));

    // A plain, undecorated local BETWEEN the over-aligned ones: the frame must
    // still place ordinary slots correctly around the padded ones.
    char plain[8];

    if (((unsigned long long)&small_std[0]) % A_SMALL)   return 1;
    if (((unsigned long long)&cap_std[0])   % A_OLD_CAP) return 2;
    if (((unsigned long long)&over_std[0])  % A_OVER)    return 3;
    if (((unsigned long long)&page_std[0])  % A_PAGE)    return 4;

    if (((unsigned long long)&small_gnu[0]) % A_SMALL)   return 5;
    if (((unsigned long long)&cap_gnu[0])   % A_OLD_CAP) return 6;
    if (((unsigned long long)&over_gnu[0])  % A_OVER)    return 7;
    if (((unsigned long long)&page_gnu[0])  % A_PAGE)    return 8;

    // Every slot must be independently writable and readable — an alignment
    // achieved by OVERLAPPING two slots would satisfy the arithmetic above and
    // be caught only here.
    small_std[0] = (char)(seed + 1);
    cap_std[0]   = (char)(seed + 2);
    over_std[0]  = (char)(seed + 3);
    page_std[0]  = (char)(seed + 4);
    small_gnu[0] = (char)(seed + 5);
    cap_gnu[0]   = (char)(seed + 6);
    over_gnu[0]  = (char)(seed + 7);
    page_gnu[0]  = (char)(seed + 8);
    plain[0]     = (char)(seed + 9);

    // Touch the FAR END of each over-aligned buffer too, so a slot that was
    // aligned but under-sized is caught.
    over_std[A_OVER - 1]  = 1;
    page_std[A_PAGE - 1]  = 1;
    over_gnu[A_OVER - 1]  = 1;
    page_gnu[A_PAGE - 1]  = 1;

    if (small_std[0] != (char)(seed + 1)) return 9;
    if (cap_std[0]   != (char)(seed + 2)) return 10;
    if (over_std[0]  != (char)(seed + 3)) return 11;
    if (page_std[0]  != (char)(seed + 4)) return 12;
    if (small_gnu[0] != (char)(seed + 5)) return 13;
    if (cap_gnu[0]   != (char)(seed + 6)) return 14;
    if (over_gnu[0]  != (char)(seed + 7)) return 15;
    if (page_gnu[0]  != (char)(seed + 8)) return 16;
    if (plain[0]     != (char)(seed + 9)) return 17;

    if (over_std[A_OVER - 1] != 1 || page_std[A_PAGE - 1] != 1) return 18;
    if (over_gnu[A_OVER - 1] != 1 || page_gnu[A_PAGE - 1] != 1) return 19;

    return 0;
}

int main(void) {
    int const bad = check(0);
    if (bad != 0) return 50 + bad;
    // A SECOND call, so the over-aligned frame is set up and torn down twice —
    // a prologue that aligned the frame by accident on the first entry would
    // not survive a second one with a different incoming stack pointer.
    char pad[3];
    pad[0] = 1;
    int const bad2 = check(pad[0]);
    if (bad2 != 0) return 80 + bad2;
    return 42;
}
