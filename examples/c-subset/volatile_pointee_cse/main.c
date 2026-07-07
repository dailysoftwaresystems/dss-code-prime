// c27 (D-CSUBSET-VOLATILE-POINTEE) — the NEGATIVE MISCOMPILE pin for a volatile
// POINTEE (the genuine red-on-disable witness, exit 3 WITH the flag, exit 2
// WITHOUT). This is the pointee-specific companion to `volatile_member` (which
// pins a volatile scalar GLOBAL): it proves the volatile rides the DEREF of a
// `Ptr<VolatileQual(int)>`, the form c21 could not express.
//
// `cell` is a plain global; `p` is a `volatile int *` aimed at it. The two `*p`
// reads straddle an opaque `poke()` CALL. `poke` is padded ABOVE the inline
// threshold so it stays a real CALL (not inlined to a store), and CSE's
// load-admission scans only STORES — a Call is NOT a clobber — so for a
// NON-volatile pointee the optimizer would FOLD the second `*p` onto the first:
// b == a == 1 and the program returns 2 (the miscompile). The `volatile` qualifier
// on the POINTEE makes each `*p` load carry MirInstFlags::Volatile, so CSE skips
// the fold (cse.cpp): the second read RE-fetches 2 and the program returns 1 + 2
// == 3. The release pipeline arm runs the real optimizer (Inlining/CSE/Mem2Reg),
// so this is an end-to-end witness, not an IR-shape assertion.
int cell;
int sink;

void poke(int x) {            // opaque write — padded > inline threshold, stays a call
    cell = x;
    sink = x + 1;  sink = x + 2;  sink = x + 3;  sink = x + 4;  sink = x + 5;
    sink = x + 6;  sink = x + 7;  sink = x + 8;  sink = x + 9;  sink = x + 10;
    sink = x + 11; sink = x + 12; sink = x + 13; sink = x + 14; sink = x + 15;
    sink = x + 16; sink = x + 17; sink = x + 18; sink = x + 19; sink = x + 20;
}

int main(void) {
    volatile int *p = &cell;  // pointer to a volatile-qualified pointee (c27)
    poke(1);
    int a = *p;               // volatile read through the pointee — fetches 1
    poke(2);
    int b = *p;               // volatile read — must RE-fetch 2 (NOT CSE'd to a)
    return a + b;             // volatile => 3 ; folded (flag missing) => 2
}
