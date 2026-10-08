// <math.h>'s float constants are MACROS of their own type, on every pair, as on every
// reference (P69 round 4, D-FFI-DESCRIPTOR-FLOAT-CONSTANTS-INVISIBLE-TO-THE-PREPROCESSOR).
// C 7.12p3-5: INFINITY and NAN are constant expressions of type float, HUGE_VAL a positive
// double. Before the splice none of the three was a macro (`#ifdef INFINITY` was false on
// every pair), INFINITY was a double, and NAN was absent. Each check below returns its own
// code; 42 means every one held.
#include <math.h>
#include <string.h>

#ifdef INFINITY
#define HAVE_INFINITY 1
#else
#define HAVE_INFINITY 0
#endif
#ifdef NAN
#define HAVE_NAN 1
#else
#define HAVE_NAN 0
#endif
#ifdef HUGE_VAL
#define HAVE_HUGE_VAL 1
#else
#define HAVE_HUGE_VAL 0
#endif

// A program's own fallback, the shape the missing macro silently took over: with the
// header it must never be taken.
#ifndef INFINITY
#define INFINITY (1.0 / 0.0)
#define TOOK_THE_FALLBACK 1
#else
#define TOOK_THE_FALLBACK 0
#endif

// Constant expressions: static initializers.
static float const  kInf    = INFINITY;
static float const  kNegInf = -INFINITY;
static float const  kNan    = NAN;
static double const kHuge   = HUGE_VAL;

static unsigned int bits32(float f) {
    unsigned int u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static unsigned long long bits64(double d) {
    unsigned long long u;
    memcpy(&u, &d, sizeof u);
    return u;
}

int main(void) {
    if (!HAVE_INFINITY) return 1;
    if (!HAVE_NAN) return 2;
    if (!HAVE_HUGE_VAL) return 3;
    if (TOOK_THE_FALLBACK) return 4;
    if (_Generic(INFINITY, float: 1, default: 0) != 1) return 5;
    if (_Generic(NAN, float: 1, default: 0) != 1) return 6;
    if (_Generic(HUGE_VAL, double: 1, default: 0) != 1) return 7;
    if (sizeof INFINITY != sizeof(float) || sizeof NAN != sizeof(float)) return 8;
    if (sizeof HUGE_VAL != sizeof(double)) return 9;
    // The values, by bit pattern: +inf as a float, the positive quiet NaN with the empty
    // payload every reference's NAN is (0x7fc00000), +inf as a double.
    if (bits32(kInf) != 0x7f800000u) return 10;
    if (bits32(kNegInf) != 0xff800000u) return 11;
    if (bits32(kNan) != 0x7fc00000u) return 12;
    if (bits64(kHuge) != 0x7ff0000000000000ull) return 13;
    // The same values through a run-time use of the macro, not only through a static.
    volatile float vi = INFINITY;
    volatile float vn = NAN;
    volatile double vh = HUGE_VAL;
    if (bits32(vi) != 0x7f800000u || bits32(vn) != 0x7fc00000u) return 14;
    if (bits64(vh) != 0x7ff0000000000000ull) return 15;
    // A NaN compares unequal to everything, itself included; an infinity is above every
    // finite float.
    if (vn == vn) return 16;
    if (!(vi > 3.4e38f)) return 17;
    if (!((double)INFINITY == HUGE_VAL)) return 18;
    return 42;
}
