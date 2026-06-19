// FC12c (D-FC12C-AAPCS64-VARIADIC-CALLEE) FP-OVERFLOW witness: an AAPCS64 variadic
// callee whose DOUBLE varargs EXCEED the 8 VR arg registers (v0..v7), so the 9th is
// read from __stack — the va_arg(double) __stack overflow arm. This is the runtime
// witness varargs_aapcs64_double (3 register-resident doubles) cannot give, and the
// FP complement of the integer-only varargs_aapcs64_overflow.
//
// The fixed `n` is an INTEGER (x0), so ALL 8 VRs (v0..v7) are free for doubles. main
// passes 9 doubles: doubles 1-8 ride v0..v7 (the __vr_offs cursor walks -128,-112,...,
// -16), and the 9th sees __vr_offs == 0 (NOT < 0) and therefore takes va_arg's __stack
// arm — reading the double from the incoming-stack-arg area and bumping __stack by
// gpSlotBytes(8), the NSAA round-up quantum (NOT fpSlotBytes=16, even for a double).
//
// Values (all exactly representable in binary double, so the sum is exact):
//   10.5 + 20.25 + 19.25 + 0.5 + 0.25 + 0.25 + 1.0 + 2.0 + 6.0 = 60.0
//   running: 10.5, 30.75, 50.0, 50.5, 50.75, 51.0, 52.0, 54.0, 60.0
// (int)60.0 = 60 -> exit 60. The 9th double (6.0) is the __stack-resident one; a wrong
// FP-overflow geometry (bad __stack base, wrong pointer bump, a missed register cutover)
// drops or mis-reads it and flips the exit off 60. Runs under qemu-aarch64.

double sum_d(int n, ...) {
    va_list ap;
    va_start(ap, n);
    double t = 0.0;
    for (int i = 0; i < n; i = i + 1) {
        t = t + va_arg(ap, double);
    }
    va_end(ap);
    return t;
}

int main(void) {
    return (int)sum_d(9, 10.5, 20.25, 19.25, 0.5, 0.25, 0.25, 1.0, 2.0, 6.0);
}
