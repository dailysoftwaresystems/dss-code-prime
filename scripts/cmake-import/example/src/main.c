#include "mathlib.h"

/* scaled_square(7) = (7 * 7) * SCALE. With the build's -DSCALE=2 that is
   49 * 2 = 98, so the process exits 98 — a value that proves the imported
   define flowed all the way through cmake-import -> --project -> codegen. */
int main(void) {
    int r = scaled_square(7);
    return r % 256;
}
