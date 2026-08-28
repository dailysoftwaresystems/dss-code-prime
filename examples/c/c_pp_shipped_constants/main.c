// D-FFI-DESCRIPTOR-CONSTANTS-INVISIBLE-TO-THE-PREPROCESSOR — a shipped
// descriptor's `constants` surface must reach the PREPROCESSOR, not only the
// semantic seam.
//
// ★★ EVERY CELL IS EXIT-CODE DISCRIMINATING IN BOTH DIRECTIONS. This defect
// selected the WRONG BRANCH and then compiled it perfectly — rc=0, no
// diagnostic, a different program. An example that only had to COMPILE would be
// structurally blind to it, which is precisely how the surface went four months
// without anyone noticing that `#if INT_MIN < 0` was false. So each `#if` writes
// a DIFFERENT value per arm and `main` returns 42 only if every cell chose the
// arm gcc AND clang choose.
//
// ✔MEASURED 2026-08-27 against gcc 13.3.0 `-std=c2x` and clang 18.1.3
// `-std=c2x`, probed SEPARATELY, each cell's taken arm read out of the EMITTED
// OBJECT with `nm` rather than from an exit code — an exit code cannot see a
// wrong branch taken by the compiler that produced it.

#include <limits.h>
#include <stdint.h>
#include <stdio.h>

// ── THE THREE ORIGINALLY-MEASURED WRONG ARMS ────────────────────────────────
// gcc: TRUE. clang: TRUE. DSS before the fix: FALSE, on all three, silently.

#if UINT_MAX > INT_MAX
#define C_UMAX_GT_IMAX 1
#else
#define C_UMAX_GT_IMAX 0
#endif

#if UINT_MAX > 0
#define C_UMAX_POSITIVE 1
#else
#define C_UMAX_POSITIVE 0
#endif

// ℹ The cell that makes the MECHANISM unmistakable: no arithmetic subtlety can
// explain `INT_MIN < 0` being false, only an undefined name folding to 0.
#if INT_MIN < 0
#define C_IMIN_NEGATIVE 1
#else
#define C_IMIN_NEGATIVE 0
#endif

// ── DEFINEDNESS: the discriminating pair that LOCATED the defect ─────────────
// `UINT_MAX` is a `constants` row; `UINT64_C` is a `macros` row. Before the fix
// the first was FALSE and the second TRUE, which is how the gap was localised to
// the SURFACE rather than to the descriptor→preprocessor bridge.

#if defined(UINT_MAX)
#define C_DEFINED_CONSTANT 1
#else
#define C_DEFINED_CONSTANT 0
#endif

// THE POSITIVE CONTROL: the surface that already worked must still work, so a
// fix that broke the macro splice while adding the constant splice is caught.
#if defined(UINT64_C)
#define C_DEFINED_MACRO 1
#else
#define C_DEFINED_MACRO 0
#endif

// ── SIGNEDNESS: the cell a decimal-only implementation FAILS ─────────────────
// ★★ `-1 < UINT_MAX` is FALSE in gcc and clang — `UINT_MAX` is unsigned, so the
// `-1` converts to `uintmax_t`. Spell the constant `4294967295` instead of
// `4294967295u` and this arm FLIPS. It is the only cell here that distinguishes
// "the value reached the preprocessor" from "the value AND its signedness did",
// and it is the reason the splice asks the language's own literal ladder for a
// suffix instead of printing a decimal.
#if -1 < UINT_MAX
#define C_UNSIGNED_CONVERTS 0
#else
#define C_UNSIGNED_CONVERTS 1
#endif

// The same property from the other side: a SIGNED constant must NOT convert its
// operand. Without this, an implementation that spelled everything unsigned
// would pass the cell above.
#if -1 < INT_MAX
#define C_SIGNED_STAYS_SIGNED 1
#else
#define C_SIGNED_STAYS_SIGNED 0
#endif

// ── ARITHMETIC over spliced constants, and the widths ───────────────────────

#if CHAR_BIT == 8
#define C_CHAR_BIT 1
#else
#define C_CHAR_BIT 0
#endif

#if UCHAR_MAX == 255
#define C_UCHAR_MAX 1
#else
#define C_UCHAR_MAX 0
#endif

#if SHRT_MIN < 0 && SHRT_MAX > 0
#define C_SHRT_RANGE 1
#else
#define C_SHRT_RANGE 0
#endif

#if SCHAR_MIN == -128 && SCHAR_MAX == 127
#define C_SCHAR_RANGE 1
#else
#define C_SCHAR_RANGE 0
#endif

// INT_MIN spelled naively as `-2147483648` is unary minus on a literal no `int`
// candidate holds; the compensated form both references use keeps the value.
#if INT_MIN + INT_MAX == -1
#define C_IMIN_PLUS_IMAX 1
#else
#define C_IMIN_PLUS_IMAX 0
#endif

// UINT_MAX must be the full 32-bit value, not a truncated or sign-extended one.
#if UINT_MAX == 4294967295u
#define C_UMAX_VALUE 1
#else
#define C_UMAX_VALUE 0
#endif

// A constant from a DIFFERENT shipped header, so the fix is not limited to the
// one descriptor it was found on. `EOF` is a `stdio.json` constant.
#if EOF == -1
#define C_EOF_VALUE 1
#else
#define C_EOF_VALUE 0
#endif

// ── `#elif` and `#ifdef`, so the whole conditional family sees the name ──────
#if UINT_MAX < 100
#define C_ELIF_ARM 1
#elif INT_MIN < 0
#define C_ELIF_ARM 2
#else
#define C_ELIF_ARM 3
#endif

#ifdef CHAR_BIT
#define C_IFDEF 1
#else
#define C_IFDEF 0
#endif

// ── THE CONTROL, with no shipped constant in it at all, so this example can
// demonstrate AGREEMENT rather than only ever reporting divergence. ──────────
#if 2 + 2 == 4
#define C_CONTROL 1
#else
#define C_CONTROL 0
#endif

// ★ THE SEMANTIC SEAM MUST STILL WORK. This row ADDS a preprocessor surface, it
// does not relocate the semantic one — a `constants` entry is also a
// constant-expression participant. A wrong type here is a COMPILE error (a
// negative array bound), so this cell is loud in a second way.
static int const kCharBitArray[CHAR_BIT == 8 ? 1 : -1] = {0};

// ★ AND THE PHASE-7 TYPE MUST BE UNCHANGED. `_Generic` is the only observable
// that separates "the preprocessor sees the value" from "the preprocessor sees
// the value AND the language still types it as the descriptor declared". A
// naive `-2147483648` spelling types INT_MIN as `long` and this cell goes to 0.
// gcc: 1. clang: 1.
static int typesAreUnchanged(void) {
    int ok = 1;
    ok &= _Generic(INT_MIN,   int: 1,          default: 0);
    ok &= _Generic(INT_MAX,   int: 1,          default: 0);
    ok &= _Generic(UINT_MAX,  unsigned int: 1, default: 0);
    ok &= _Generic(CHAR_BIT,  int: 1,          default: 0);
    ok &= _Generic(SCHAR_MIN, int: 1,          default: 0);
    ok &= _Generic(EOF,       int: 1,          default: 0);
    return ok;
}

int main(void) {
    int ok = 0;
    ok += C_UMAX_GT_IMAX;        /*  1 */
    ok += C_UMAX_POSITIVE;       /*  2 */
    ok += C_IMIN_NEGATIVE;       /*  3 */
    ok += C_DEFINED_CONSTANT;    /*  4 */
    ok += C_DEFINED_MACRO;       /*  5 */
    ok += C_UNSIGNED_CONVERTS;   /*  6 */
    ok += C_SIGNED_STAYS_SIGNED; /*  7 */
    ok += C_CHAR_BIT;            /*  8 */
    ok += C_UCHAR_MAX;           /*  9 */
    ok += C_SHRT_RANGE;          /* 10 */
    ok += C_SCHAR_RANGE;         /* 11 */
    ok += C_IMIN_PLUS_IMAX;      /* 12 */
    ok += C_UMAX_VALUE;          /* 13 */
    ok += C_EOF_VALUE;           /* 14 */
    ok += C_IFDEF;               /* 15 */
    ok += C_CONTROL;             /* 16 */

    // The `#elif` chain contributes its ARM NUMBER, so choosing arm 1 or arm 3
    // is distinguishable from choosing arm 2 rather than merely "not 2".
    ok += C_ELIF_ARM;            /* +2 => 18 */

    // The semantic seam and the phase-7 types, each worth one.
    ok += (int)(sizeof kCharBitArray / sizeof kCharBitArray[0]);  /* +1 => 19 */
    ok += typesAreUnchanged();                                    /* +1 => 20 */

    // 20 correct cells. Scaled and offset so a single wrong branch cannot land
    // on 42 by arithmetic accident: 20*2 + 2 == 42.
    return ok * 2 + 2;
}
