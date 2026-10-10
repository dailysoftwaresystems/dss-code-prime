#include <math.h>

// c52 (D-FFI-MATH-INFINITY): INFINITY ships from <math.h> — since P69 round 4
// (D-FFI-DESCRIPTOR-FLOAT-CONSTANTS-INVISIBLE-TO-THE-PREPROCESSOR) a MACRO of
// type float, as C 7.12p4 says (an f64 named constant before).
// `(double)INFINITY` is f64 +inf either way; `+inf > 1e308` is true, so this
// returns 42. RED-ON-DISABLE: remove the math.json `floatConstants` INFINITY
// entry -> S0001 (undeclared).
int main(void) {
    double x = (double)INFINITY;
    return (x > 1e308) ? 42 : 1;
}
