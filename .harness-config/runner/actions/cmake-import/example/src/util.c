#include "mathlib.h"

/* SCALE is supplied by the build via -DSCALE=2 (target_compile_definitions).
   The fallback keeps the file compilable on its own. */
#ifndef SCALE
#define SCALE 1
#endif

int square(int x) {
    return x * x;
}

int scaled_square(int x) {
    return square(x) * SCALE;
}
