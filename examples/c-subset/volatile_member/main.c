// c21 (D-CSUBSET-VOLATILE-QUALIFIER) — the negative MISCOMPILE pin (the genuine
// red-on-disable witness verified in WSL: exit 3 WITH the flag, exit 2 WITHOUT).
//
// `g` is a `volatile int`. The two `g` reads straddle an opaque `poke()` write.
// `poke` is padded ABOVE the inline threshold so it stays a real CALL (not
// inlined to a store), and CSE's load-admission scans only STORES — a Call is
// NOT a clobber — so for a NON-volatile `g` the optimizer FOLDS the second load
// onto the first: b == a == 1 and the program returns 2 (the miscompile). The
// `volatile` flag on each load makes CSE skip the fold (cse.cpp), so the second
// read RE-fetches 2 and the program returns 1 + 2 == 3. The release pipeline arm
// runs the real optimizer (Inlining/CSE/Mem2Reg), so this is an end-to-end
// witness, not an IR-shape assertion. (A volatile struct MEMBER's flag is pinned
// directly by the MIR test `VolatileMemberLoadFlaggedNonVolatileSiblingNot`; a
// member's GEP-based loads do not CSE-fold across a call, so the scalar-global
// form above is what distinguishes fold-vs-not at the exit-code level.)
volatile int g;
int sink;

void poke(int x) {            // opaque write — padded > inline threshold, stays a call
    g = x;
    sink = x + 1;  sink = x + 2;  sink = x + 3;  sink = x + 4;  sink = x + 5;
    sink = x + 6;  sink = x + 7;  sink = x + 8;  sink = x + 9;  sink = x + 10;
    sink = x + 11; sink = x + 12; sink = x + 13; sink = x + 14; sink = x + 15;
    sink = x + 16; sink = x + 17; sink = x + 18; sink = x + 19; sink = x + 20;
}

int main(void) {
    poke(1);
    int a = g;                // volatile read — fetches 1
    poke(2);
    int b = g;                // volatile read — must RE-fetch 2 (NOT CSE'd to a)
    return a + b;             // volatile => 3 ; folded (flag missing) => 2
}
