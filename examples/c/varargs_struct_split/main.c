// FC12a-struct (D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT, the register-exhaustion SPLIT):
// an InRegisters {long,long} (2-GPR) struct vararg that CANNOT fit wholly in the
// remaining arg registers goes ENTIRELY to the overflow area via the Option-C carrier
// — SysV forbids the register/stack split, so the whole aggregate lands in memory. It
// exercises:
//
//   * THE ATOMIC-EXHAUSTION ROUTE — `combine(int n, ...)` with 1 fixed int (rdi) + 5
//     `long` varargs (rsi/rdx/rcx/r8/r9) exhausts all 6 SysV integer arg registers.
//     The trailing `struct LL {long a; long b;}` vararg needs 2 GPRs but 0 remain, so
//     the WHOLE struct is byte-copied to the overflow area (two 8-byte stores), NOT
//     split (1 piece in r9... — there is none free) and NOT half-on-the-stack.
//
//   * THE va_arg READ ACROSS THE CUTOVER — the 5 scalar longs are read (1..5 from the
//     register-save-area: gp_offset 8..40), then the {long,long} struct's `va_arg`
//     sees gp_offset == 48 (== limit) for BOTH pieces and takes the OVERFLOW arm,
//     reading a/b from consecutive overflow eightbytes and bumping by roundUp(16,8)=16.
//
// The 5 longs AND the struct's 2 fields are summed (anti-folded — all PASSED as
// varargs), so a wrong split / wrong overflow base flips the exit.
//
//   5 longs: 1+2+3+4+5 = 15;  struct LL {40, 5}: 40 + 5 = 45;  total = 60 -> exit 60.
//
// Runs natively on the x86_64-Linux CI leg.

struct LL { long a; long b; };   // 16B -> InRegisters (2 GPR)

long combine(int n, ...) {
    va_list ap;
    va_start(ap, n);
    long total = 0;
    // First `n` scalar long varargs (here 5) drain the integer arg registers.
    for (int i = 0; i < n; i = i + 1) {
        total = total + va_arg(ap, long);
    }
    // The trailing struct vararg — its 2 GPR pieces cannot fit, so the whole struct
    // sits by value in the overflow area.
    struct LL p = va_arg(ap, struct LL);
    total = total + p.a + p.b;
    va_end(ap);
    return total;
}

int main(void) {
    struct LL p = {40, 5};
    // n=5 scalar longs (1..5) exhaust the GPR pool, then the {long,long} struct.
    return (int)combine(5, 1L, 2L, 3L, 4L, 5L, p);   // 15 + 45 = 60
}
