// FC12a-core OVERFLOW witness (D-FC12A-VARIADIC-CALLEE): a variadic callee whose
// varargs EXCEED the SysV integer arg registers, so the later ones are read from
// `overflow_arg_area` (the stack) — not reg_save_area. This is the runtime witness
// the register-only `varargs_sum` corpus cannot give.
//
// `sum(int n, ...)`: the fixed `n` occupies rdi, leaving 5 integer arg registers
// (rsi/rdx/rcx/r8/r9) for varargs. So varargs 1-5 are register-resident
// (gp_offset 8,16,24,32,40) and the 6th+ vararg has gp_offset == 48 (== gpOffsetLimit,
// not < 48) and therefore takes va_arg's OVERFLOW arm, reading from overflow_arg_area
// and bumping that pointer. `main` passes 10 varargs (1..10): 1-5 from registers,
// 6-10 from the overflow area. Sum = 55 → exit 55. A wrong overflow geometry (bad
// overflow_arg_area base, wrong pointer bump, or a misclassified register cutover)
// changes the total and flips the exit. NOTE: `n` is the only fixed param (1 GPR),
// so the fixed-params-overflow-to-stack sub-case (D-FC12A-VARIADIC-OVERFLOW-FIXED-STACK-ARGS)
// is NOT exercised here — this witnesses the common overflow read path instead.
// ⚠ THIS NOTE USED TO CALL THAT SUB-CASE `fail-loud` AND THAT HAD BEEN FALSE
// SINCE 2026-06-20, when the row closed: a variadic callee whose FIXED scalar
// params overflow the arg registers LOWERS, and va_start bakes the fixed-stack
// displacement into the `VaOverflowArgAreaAddr` payload. ✔RE-MEASURED at the
// CLI: an 8-fixed-param variadic callee compiles rc=0 on
// x86_64:pe64-x86_64-windows-exec and x86_64:elf64-x86_64-linux-exec at debug
// AND release, and RUNS with the right answer. The sub-case is simply covered
// ELSEWHERE — `varargs_overflow_fixed_stack` is its direct SysV twin and
// `varargs_aapcs64_overflow_fixed_stack` the AAPCS64 one — which is the true
// reason to say it is not exercised here. Runs natively on the x86_64-Linux CI
// leg.

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
