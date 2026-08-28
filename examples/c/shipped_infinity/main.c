#include <math.h>

// c52 (D-FFI-MATH-INFINITY): INFINITY ships as an f64 float constant from
// <math.h>. `(double)INFINITY` is a runtime f64 +inf value (not a constant
// expression); `+inf > 1e308` is true, so this returns 42. RED-ON-DISABLE:
// remove the math.json `floatConstants` INFINITY entry -> S0001 (undeclared).
int main(void) {
    double x = (double)INFINITY;
    return (x > 1e308) ? 42 : 1;
}
