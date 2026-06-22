// FC12c (D-FC12C-AAPCS64-VARIADIC-CALLEE): the VR / __vr_offs variadic-callee path.
// `sum_d` takes one fixed int `n` then `n` variadic DOUBLES; va_arg(ap, double) reads
// from the VR register-save-area via the __vr_offs cursor (NOT the GR cursor). `main`
// calls sum_d(3, 10.5, 20.25, 19.25) -> 50.0 -> (int) -> exit 50.
//
// This pins the parts varargs_aapcs64_sum can't reach:
//   * va_arg's FP classification (scalarArgClass(double) == Fpr -> the __vr_offs
//     cursor + __vr_top + fpSlotBytes(16) bump, vs the GR path).
//   * va_start's __vr_offs init = -(8 - fixedFpr(0)) * 16 = -128 (the VR cursor starts
//     at the head of the VR save block; __vr_top points PAST it).
//   * The LIR prologue's VR-register spill (v0..v7) into the VR block via the NEW
//     fstur_q (STUR Qt) — 16-byte slots (a D-form 8-byte store would spill only the
//     low half and va_arg(double) would read a half-clobbered slot for v1+).
//   * The stack-bump-by-8 detail: even a double on the __stack overflow path bumps by
//     gpSlotBytes(8), the NSAA round-up quantum (not fpSlotBytes=16) — though here all
//     three doubles are register-resident (v0..v2).
//
// The fixed `n` is an INTEGER (x0), so the GR + VR save areas are BOTH exercised in
// one function. Runs under qemu-aarch64 (the linux-arm64 CI leg / local qemu).

int sum_d(int n, ...) {
    va_list ap;
    va_start(ap, n);
    double t = 0.0;
    for (int i = 0; i < n; i = i + 1) {
        t = t + va_arg(ap, double);
    }
    va_end(ap);
    return (int)t;
}

int main(void) {
    return sum_d(3, 10.5, 20.25, 19.25);
}
