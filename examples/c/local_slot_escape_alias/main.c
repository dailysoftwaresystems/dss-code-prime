// D-OPT-MEMSSA-WALK-PAST-PRECISION — the RUNTIME pin for `mirMayAlias`
// Rule 3b (the pointer-provenance + escape substrate, opt/analysis/mir_escape.hpp).
//
// Rule 3b lets a Load from a NON-ESCAPED local slot be reused across a Store
// through a pointer of external provenance. That is the precision win; it is
// also the one direction that can license a STALE LOAD, so both halves are
// written as a RELOAD PAIR inside a loop — a Load, a Store, the same Load
// again — which is the exact shape CSE's Load-admission gate decides, with the
// back edge included. Each half is the other's control:
//
//   `hot` — `keep`'s address never leaves the function, so the Store through
//     the parameter `p` provably cannot reach it and the second `keep[i & 7]`
//     may be reused. Every type rule answers Maybe on this pair (`int *`
//     against `int[8]`, identical pointee types); only provenance separates
//     it. `sink == 2` in main proves the parameter Store really executed
//     rather than being optimized away, which keeps this half from passing
//     vacuously.
//
//   `cold` — the SHAPE control. `cell`'s address IS published to a file-scope
//     pointer, and the write between its two loads goes through a value
//     LOADED back out of that pointer, whose origin is External. Rule 3b must
//     therefore NOT fire, and `b - a` must be 1 every round: the reload sees
//     the write. A reused reload here would make every round 0 and return 0
//     instead of 3.
//
// ⚠ HONEST SCOPE, MEASURED, not assumed. This example proves the two shapes
// still COMPUTE 16 under every optimized arm, and it does exercise Rule 3b:
// with Rule 3b disabled the shipped `release` image for this file changes
// (md5 e47235c5… vs 7d83c6c8… on x86_64:pe64, both exit 16). It is NOT the
// pin that goes red when the ESCAPE half is broken — disabling the forward
// escape scan emits a BYTE-IDENTICAL release image for this file, so `cold`'s
// reload never becomes a CSE candidate here. The measured red for that half
// lives at the pass tier, in
// tests/opt/test_mir_memory_clobbers.cpp::CseReusesALoadAcrossAParameterStoreOnlyWhenTheSlotStayedHome,
// which runs the real `runCse` on both shapes and reds in BOTH directions.
//
// The loop bound comes from a MUTABLE file-scope int rather than a literal:
// with a literal the whole program folds to its answer before any Load
// reaches CSE. MEASURED — a straight-line version of this file emitted a
// byte-identical release image with Rule 3b disabled, i.e. exercised nothing.
//
// Exit code: 12 + 3 + 1 = 16, identical under every optimized arm.

static int rounds = 3;
static int *pub;

// The NON-ESCAPING half. `keep` is only ever indexed and stored THROUGH; its
// address is never handed to anything.
static int hot(int *p, int n)
{
    int keep[8];
    for (int i = 0; i < 8; ++i) {
        keep[i] = i + 1;
    }
    int total = 0;
    for (int i = 0; i < n; ++i) {
        int a = keep[i & 7];    /* Load #1 */
        *p = i;                 /* a Store through a PARAMETER — cannot reach keep */
        int b = keep[i & 7];    /* Load #2 — reusable ONLY under Rule 3b */
        total += a + b;
    }
    return total;               /* 2 + 4 + 6 = 12 */
}

// The ESCAPING half — the negative control that turns a missed escape into a
// wrong exit code rather than into silence.
static int cold(int n)
{
    int cell[8];
    for (int i = 0; i < 8; ++i) {
        cell[i] = 0;
    }
    pub = &cell[0];             /* THE ESCAPE: the slot's address is published */
    int total = 0;
    for (int i = 0; i < n; ++i) {
        int a = cell[0];        /* Load #1 */
        *pub = i + 1;           /* an External-origin pointer that DOES alias it */
        int b = cell[0];        /* Load #2 — MUST observe i+1, never the stale a */
        total += b - a;         /* 1 each round; 0 each round if reused */
    }
    pub = 0;                    /* nothing dangling survives the return */
    return total;               /* 3 */
}

int main(void)
{
    int sink = 0;
    int a = hot(&sink, rounds);
    int b = cold(rounds);
    return a + b + (sink == 2 ? 1 : 0);
}
