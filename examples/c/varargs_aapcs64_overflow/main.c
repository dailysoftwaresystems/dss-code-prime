// FC12c (D-FC12C-AAPCS64-VARIADIC-CALLEE) OVERFLOW witness: an AAPCS64 variadic callee
// whose INTEGER varargs EXCEED the 7 remaining GR arg registers, so the later ones are
// read from __stack (the incoming-stack-arg area) — not the GR register-save-area.
// This is the runtime witness the register-only varargs_aapcs64_sum corpus cannot give.
//
// `sum(int n, ...)`: the fixed `n` occupies x0, leaving 7 integer arg registers
// (x1..x7) for varargs. So varargs 1-7 are register-resident (__gr_offs walks
// -56,-48,...,-8) and the 8th+ vararg sees __gr_offs == 0 (NOT < 0) and therefore
// takes va_arg's __stack arm, reading from __stack and bumping that pointer by 8 (the
// NSAA quantum). `main` passes 10 varargs (1..10): 1-7 from registers, 8-10 from the
// __stack overflow area. Sum = 55 -> exit 55. A wrong overflow geometry (a bad __stack
// base, a wrong pointer bump, a missed register cutover, or — BLOCKER-2 — a cursor that
// is NOT sign-extended so the reg arm computes a wild address) changes the total and
// flips the exit. `n` is the only fixed param (1 GR), so the fixed-params-overflow-to-
// stack sub-case stays fail-loud and is NOT exercised here. Runs under qemu-aarch64.

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
    return sum(10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
}
