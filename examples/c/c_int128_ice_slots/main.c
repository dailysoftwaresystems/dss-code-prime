// D-CSUBSET-INT128-ICE-CONTEXT-REFUSED / D-CE-ASINT64-REJECTS-BY-WIDTH-NOT-MAGNITUDE.
//
// A 128-bit (or any N>64 bit-precise) CONSTANT whose VALUE fits in an int64 is a
// perfectly good integer constant expression, and C admits one in every 64-bit ICE
// slot: an array bound (6.7.6.2p1), an enumerator (6.7.2.2p2) and a bit-field width
// (6.7.2.1p4). MEASURED before this landed: every shape below was REFUSED --
// `int a[(__int128)2 + 1];` gave S_NonConstantArrayLength, `enum { EA = (__int128)7 }`
// gave S_EnumeratorNotConstant, a `(__int128)3` bit-field width gave S001F -- while
// gcc 13.3.0 (`-std=c2x`) and clang 18.1.3 (`-std=c23`), probed SEPARATELY, accept
// all of them. The refusal was the const-evaluator's int64 bridge testing the
// value's declared WIDTH instead of its MAGNITUDE.
//
// ★ WHY THE ASSERTIONS ARE SHAPED THE WAY THEY ARE. "It compiled" proves nothing
// here: the whole failure mode this file guards against is a wide value SILENTLY
// contributing its low 64 bits, and a bound/enumerator/width that is merely
// PRESENT would pass. So every term below asserts the resulting VALUE -- the array
// LENGTH through `sizeof`, the enumerator through its arithmetic, the bit-field
// through a stored value that only fits at the declared width -- and the terms are
// multiplicatively gated, so one wrong value drives the exit code away from 42
// instead of being absorbed by its neighbours.
//
// ⚠ THE COMPLEMENTARY HALF IS A SEPARATE, DELIBERATELY NON-RUNNABLE EXAMPLE:
// `examples/c/c_int128_ice_slot_overflow_error` pins that a value which does NOT
// fit still FAILS LOUD. Both halves are required -- accepting the fitting values
// without refusing the non-fitting ones would be the silent truncation this whole
// row exists to prevent, and it would look green here.
//
// ⓘ RUNTIME, NOT `_Static_assert`: a failed static assertion ABORTS translation, so
// a term computed from one could never report a runtime failure -- and it would
// mask every term after it. The compile-time facts that cannot be observed any
// other way (the array LENGTHS) are read back through `sizeof` at runtime instead.

// ── 1. ARRAY BOUNDS from a wide-but-fitting constant ─────────────────────────
// The bound is a BARE wide value in `a` (no narrowing cast anywhere), which is the
// exact shape the row named: nothing in the source says "narrow me".
int a[(__int128)2 + 1];                 /* 3  */
int b[(__uint128_t)3];                  /* 3  */
int c[(_BitInt(128))5];                 /* 5  */
int d[(_BitInt(200))6];                 /* 6  — 4 limbs, so the limb scan is exercised */
int e[((__uint128_t)1 << 64) >> 62];    /* 4  — the value passes THROUGH >64 bits and
                                               comes back down; a fold that gave up at
                                               the first wide intermediate cannot do this */
int f[(__int128)-3 + 10];               /* 7  — a NEGATIVE wide intermediate, which the
                                               sign-extension half of the bridge owns */

// ── 2. ENUMERATORS from a wide-but-fitting constant ──────────────────────────
enum E {
    EA = (__int128)7,
    EB = (__uint128_t)40,
    EC = (_BitInt(128))-5,              /* negative, from a wide bit-precise value */
    ED = ((__int128)1 << 40) >> 38      /* 4 — again via a >64-bit intermediate */
};

// ── 3. BIT-FIELD WIDTHS from a wide-but-fitting constant ─────────────────────
// The widths are chosen so a WRONG width cannot store the value put in it: `x` at
// 3 bits holds 5, `y` at 29 bits holds 100000000 (needs 27 bits), and the two must
// pack into ONE 4-byte unit. A width silently taken as something else either loses
// the value or changes `sizeof`.
struct Packed {
    unsigned x : (__int128)3;
    unsigned y : (__uint128_t)29;
};

int main(void) {
    int t1 = 0, t2 = 0, t3 = 0, t4 = 0;

    // t1 — every array LENGTH is the value the wide constant denotes.
    t1 = (sizeof(a) / sizeof(a[0]) == 3)
      && (sizeof(b) / sizeof(b[0]) == 3)
      && (sizeof(c) / sizeof(c[0]) == 5)
      && (sizeof(d) / sizeof(d[0]) == 6)
      && (sizeof(e) / sizeof(e[0]) == 4)
      && (sizeof(f) / sizeof(f[0]) == 7);

    // t2 — the enumerators carry their values, checked by ARITHMETIC (not by
    // `EA == 7` alone, which a constant folded to the right thing by accident would
    // also satisfy): the four are combined so any single wrong one shows.
    t2 = (EA + EB == 47) && (EC == -5) && (ED == 4) && (EA * ED + EB == 68);

    // t3 — the bit-fields hold values that only fit at the DECLARED widths, and
    // the struct packs into a single 4-byte unit.
    {
        struct Packed p;
        p.x = 5u;
        p.y = 100000000u;
        t3 = (p.x == 5u) && (p.y == 100000000u) && (sizeof(struct Packed) == 4);
    }

    // t4 — the arrays are real storage, not just a `sizeof` answer: write at the
    // LAST index of each and read it back. A bound that folded to something smaller
    // than claimed would still satisfy t1 if `sizeof` and the allocation disagreed;
    // this makes them agree.
    a[2] = 11; b[2] = 12; c[4] = 13; d[5] = 14; e[3] = 15; f[6] = 16;
    t4 = (a[2] == 11) && (b[2] == 12) && (c[4] == 13)
      && (d[5] == 14) && (e[3] == 15) && (f[6] == 16);

    return t1 * t2 * t3 * t4 * 42;
}
