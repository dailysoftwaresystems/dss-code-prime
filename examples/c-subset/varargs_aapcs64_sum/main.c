// FC12c (D-FC12C-AAPCS64-VARIADIC-CALLEE): the first DSS binary that walks an
// AAPCS64 (ARM64-ELF) variadic function's INTEGER varargs via the dual-cursor
// `__va_list`. `sum` takes one fixed int `n` then `n` variadic ints; it accumulates
// them and returns the total. `main` calls sum(4, 10, 20, 30, 40) -> 100 -> exit 100.
//
// Exercises the AAPCS64 variadic-CALLEE substrate end-to-end:
//   * The semantic `__va_list` 5-field struct builtin + `va_list` typedef (the struct
//     DIRECTLY, not an array/pointer), config-gated on the c-subset `vaArgRule`.
//   * va_start initializing __stack/__gr_top/__vr_top + the NEGATIVE __gr_offs/__vr_offs
//     cursors (here __gr_offs = -(8-1)*8 = -56, since the fixed `n` consumes x0).
//   * va_arg(ap, int): the GR dual-cursor diamond — Load __gr_offs (NEGATIVE),
//     ICmpSlt(offs,0), reg arm SIGN-EXTENDS the i32 cursor before the byte Gep
//     (BLOCKER-2: a zero-extended -56 would be a +4 GiB wild address), addr =
//     __gr_top + offs, bump by 8.
//   * The LIR callconv prologue spill of x0..x7 into the GR block of the
//     register-save-area (the VR block via fstur_q is spilled too but unused here).
//
// All four varargs pass in INTEGER registers (x1..x4 — x0 holds the fixed `n`), so
// this pins the __gr_offs / __gr_top READ path. The overflow (__stack) path is
// witnessed by varargs_aapcs64_overflow (>7 varargs); the VR path by
// varargs_aapcs64_double. Runs under qemu-aarch64 (the linux-arm64 CI leg / local qemu).

int sum(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int t = 0;
    for (int i = 0; i < n; i = i + 1) {
        t = t + va_arg(ap, int);
    }
    va_end(ap);
    return t;
}

int main(void) {
    return sum(4, 10, 20, 30, 40);
}
