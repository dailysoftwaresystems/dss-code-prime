// FC12a-core (D-FC12A-VARIADIC-CALLEE): the SSE / fp_offset variadic-callee path.
// `sum_d` takes one fixed int `n` then `n` variadic DOUBLES; va_arg(ap, double)
// reads from the XMM register-save-area (fp_offset cursor, fp limit 176) instead of
// the integer save area. `main` calls sum_d(3, 10.5, 20.25, 19.25) → 50.0 → (int)
// → exit 50.
//
// This pins the parts varargs_sum can't reach:
//   * va_arg's SSE classification (scalarArgClass(double) == Fpr → the fp_offset
//     cursor + fpOffsetLimit(176) + fpSlotBytes(16) bump, vs the gp path).
//   * va_start's fp_offset init = gpOffsetLimit(48) + fixedFpr(0)*16 = 48 (the
//     cursor starts at the head of the SSE block of the save area).
//   * The LIR prologue's SSE-register spill (xmm0..xmm7) into the save area — the
//     caller passes the doubles in xmm0..xmm2 and sets al = 3.
//
// The fixed `n` is an INTEGER (rdi), so the GPR + SSE save areas are BOTH exercised
// in one function. Runs natively on the x86_64-Linux CI leg.

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
