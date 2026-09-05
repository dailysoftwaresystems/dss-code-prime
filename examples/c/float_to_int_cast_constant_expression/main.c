// C23 6.6 + 6.3.1.2 + 6.3.1.4
// ([[D-C-FLOAT-CAST-DOES-NOT-FOLD-IN-A-CONSTANT-EXPRESSION]]): a CAST of a
// floating constant to an integer type is an integer constant expression, and
// folds in every position C admits one. ✔MEASURED separately on gcc 13.3.0,
// clang 18.1.3, mingw-w64 gcc 13.2.0 and MSVC 19.51 — all four accept every line
// in this file, and gcc and mingw each RUN it to exit 42.
//
// ★ WHY THE EXIT CODE IS THE INSTRUMENT AND NOT THE STATIC ASSERTIONS. A
// compile-only arm would go green the moment `(int)1.5` folded to ANYTHING — 1,
// 2, or 0. So every enumerator below IS one of the folds, `main` is built out of
// the enumerators, and each guard line is written to cancel to exactly zero:
// fold any one of them differently and the program exits with a different number
// rather than compiling quietly.
//
// ★ THE FOLDS LIVE IN ENUMERATORS, and the array bound and bit-field width are
// then built from ENUM CONSTANTS — the shape all four references treat as a
// strict integer constant expression (writing the cast straight into an array
// bound also works, but gcc and clang then report it through their
// variable-length-array machinery, which is a different question than this one).
//
// ⚠ WHAT IS DELIBERATELY NOT HERE. A cast whose truncated value does NOT fit the
// target — `(int)1e30` — is UNDEFINED (6.3.1.4p1) and the four references
// disagree about the number they produce (INT_MAX on the three gcc/clang
// toolchains, 0 on MSVC 19.51), so DSS refuses it instead of baking one vendor's
// answer. A corpus example that must FAIL to compile has no exit code; that
// direction is pinned in the unit suite —
// `FloatCastOutOfTargetRangeIsRefusedNotSaturated` in
// tests/analysis/semantic/test_semantic_analyzer_c.

enum Folded {
    kTrunc     = (int)1.5,           /* 1        — toward zero */
    kTruncNeg  = (int)-1.5,          /* -1       — toward zero, NOT floor (-2) */
    kUnsigned  = (unsigned)3.9,      /* 3        — unsigned target */
    kChar      = (char)65.9,         /* 65       — narrow target */
    kBoolHalf  = (_Bool)0.5,         /* 1        — 6.3.1.2 compares to zero; a
                                                   one-bit truncation gives 0 */
    kBoolTwo   = (_Bool)2,           /* 1        — the integer twin of the above */
    kNarrowed  = (int)16777217.0f,   /* 16777216 — the f-suffix rounds in binary32 */
    kViaDouble = (int)(double)3      /* 3        — int -> float -> int round trip */
};

_Static_assert(kTrunc == 1,                 "a float cast is an integer constant");
_Static_assert(kTruncNeg == -1,             "truncation is toward zero");
_Static_assert(kUnsigned == 3,              "unsigned target");
_Static_assert(kChar == 65,                 "narrow target");
_Static_assert(kBoolHalf == 1,              "_Bool is a comparison, not a low bit");
_Static_assert(kBoolTwo == 1,               "_Bool of an integer, same rule");
_Static_assert(kNarrowed == 16777216,       "an f-suffixed literal is binary32");
_Static_assert(kViaDouble == 3,             "int -> double -> int");
_Static_assert((long long)1e18 == 1000000000000000000LL, "a 64-bit target");
_Static_assert((short)-300.7 == -300,       "signed narrow, negative");
_Static_assert((double)3 == 3.0,            "a cast to a FLOAT type also folds");

/* 1 + 3 + 36 == 40 elements. */
static int table[kTrunc + kUnsigned + 36];

/* 1 + 3 == 4 bits, so 15 is the widest value that survives; a 3-bit field would
   truncate it to 7 and move the exit code by 8. */
struct Packed { unsigned lane : kTrunc + kUnsigned; };

/* An index DESIGNATOR is its own const-expr consumer at a different tier — it
   folds in HIR lowering, not in the semantic pass, and it used to refuse even
   `[(int)1]`. Slot 1 is the one that must receive the 7. */
static int slots[3] = { [(int)1.5] = 7 };

/* A case LABEL is the fifth integer-constant-expression position. */
static int pick(int v) {
    switch (v) {
        case (int)2.5: return 1;
        default:       return 0;
    }
}

int main(void) {
    int total = (int)(sizeof table / sizeof table[0]);   /* 40 */
    total -= kTruncNeg;                                  /* +1 -> 41 */
    total += slots[1] / 7;                               /* +1 -> 42 */

    /* Each line below must contribute EXACTLY ZERO. They are the folds whose
       value is not already load-bearing above, written so that a wrong fold is
       an arithmetic error in the exit code rather than a silent pass. */
    struct Packed p;
    p.lane = 15;
    total += kChar - 65;
    total += kNarrowed - 16777216;
    total += kBoolHalf - 1;
    total += kBoolTwo - 1;
    total += kViaDouble - 3;
    total += (int)p.lane - 15;
    total += pick(2) - 1;
    total += (int)(long long)1e18 - (int)1000000000000000000LL;
    return total;
}
