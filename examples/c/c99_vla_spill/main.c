// D-CSUBSET-VLA C1b CRITICAL-1: a LEAF variable-length-array function under heavy
// register pressure. 26 runtime-derived locals (a volatile `base` defeats constant
// folding) are live ACROSS the `int a[n]` dynamic allocation and summed AFTER it —
// forcing the register allocator to SPILL (and later reload) around the runtime
// `sub sp, <size>`. Every spill slot lives in the FIXED frame; after the VLA moves
// SP a spill addressed off SP (instead of the frame pointer) would either read
// garbage below the array OR clobber the array's own elements. The exit code then
// flips. Both are the #1 silent-miscompile the SP->FP fixed-frame base-switch
// closes; this example is the runtime guard (the byte pins + the static completeness
// verifier are the others). LEAF (no calls), so it is in C1b frame-model scope.
//
// base == 0, so s_k == k+1;  sum(s0..s25) = 1+2+...+26 = 351.  a[5] == 5.
// Exit code MUST be 351 + 5 - 314 = 42.
int main(void) {
    volatile int base = 0;
    int s0 = base + 1,  s1 = base + 2,  s2 = base + 3,  s3 = base + 4;
    int s4 = base + 5,  s5 = base + 6,  s6 = base + 7,  s7 = base + 8;
    int s8 = base + 9,  s9 = base + 10, s10 = base + 11, s11 = base + 12;
    int s12 = base + 13, s13 = base + 14, s14 = base + 15, s15 = base + 16;
    int s16 = base + 17, s17 = base + 18, s18 = base + 19, s19 = base + 20;
    int s20 = base + 21, s21 = base + 22, s22 = base + 23, s23 = base + 24;
    int s24 = base + 25, s25 = base + 26;
    volatile int seed = 6;
    int n = seed;
    int a[n];                         // dynamic `sub sp, <size>`; s0..s25 live across
    int i;
    for (i = 0; i < n; i = i + 1) {
        a[i] = i;
    }
    // Reload every s_k AFTER the VLA sub (they were spilled around it) and read the
    // array back too — a bug in either direction corrupts this sum.
    int sum = s0 + s1 + s2 + s3 + s4 + s5 + s6 + s7 + s8 + s9
            + s10 + s11 + s12 + s13 + s14 + s15 + s16 + s17 + s18 + s19
            + s20 + s21 + s22 + s23 + s24 + s25;
    return sum + a[5] - 314;
}
