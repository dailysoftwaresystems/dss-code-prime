// FC12a-core (D-FC12A-VARIADIC-CALLEE): the first DSS binary that DEFINES a
// variadic function and walks its varargs via va_start / va_arg / va_end. `sum`
// takes one fixed int `n` (the count) then `n` variadic ints; it accumulates them
// and returns the total. `main` calls `sum(4, 10, 20, 30, 40)` → 100 → exit 100.
//
// Exercises the SysV AMD64 variadic-CALLEE substrate end-to-end:
//   * The semantic `__va_list_tag` builtin + `va_list` typedef (config-gated on the
//     c-subset `vaArgRule`), so `va_list ap;` is a real local.
//   * The HIR VaStart/VaArg/VaEnd dedicated nodes (the SizeOf precedent — the
//     va_arg TYPE is read from a TypeRef child, never value-lowered).
//   * The HIR→MIR va_start (4 field Stores into the tag) + va_arg (the reg-vs-
//     overflow diamond: gp_offset < 48 ? reg_save_area+gp_offset : overflow) + the
//     VaRegSaveAreaAddr / VaOverflowArgAreaAddr frame-address leaves.
//   * The LIR callconv prologue spill of the 6 integer arg regs (rdi..r9) into the
//     vaListLayout-sized register-save-area, gated on the function using va_start.
//
// All four varargs here pass in INTEGER registers (rsi/rdx/rcx/r8 — rdi holds the
// fixed `n`), so this pins the gp_offset / reg_save_area READ path. The OVERFLOW
// read path (varargs beyond the registers → overflow_arg_area) is witnessed
// separately by `varargs_overflow` (>5 varargs); the SSE path by `varargs_double`.
// Runs natively on the x86_64-Linux CI leg.

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
