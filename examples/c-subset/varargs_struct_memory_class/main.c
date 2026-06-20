// FC12a-struct (D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT): the first DSS binary that
// passes a >16B (MEMORY class) struct BY VALUE across the SysV AMD64 variadic
// boundary AND reads it back via `va_arg(ap, struct Big)`. SysV §3.2.3/§3.5.7
// requires a MEMORY-class aggregate to be passed ENTIRELY in the overflow (stack)
// area — UNCONDITIONALLY, even when arg registers are free — via the Option-C
// by-value-stack aggregate carrier. It exercises:
//
//   * THE CALLER FORCE-TO-STACK — `combine(int n, ...)` leaves rsi/rdx/rcx/r8/r9
//     FREE when the FIRST vararg is a `struct Big {long a,b,c;}` (24B). The struct is
//     byte-copied into the outgoing overflow area (NOT register-passed): three 8-byte
//     stores at [sp+0,+8,+16]. A regression that register-passed (or split) the struct
//     would scramble a/b/c and flip the exit.
//
//   * THE va_arg(>16B) OVERFLOW READ — each `va_arg(ap, struct Big)` dispatches to the
//     OVERFLOW-ONLY arm (no register-gather diamond): the struct's address IS
//     overflow_arg_area, and the pointer bumps by roundUp(24,8) = 24. A wrong base or
//     bump reads the wrong slot and flips the exit.
//
// All three fields are READ from the overflow slot and summed (anti-folded — the
// values are PASSED as a vararg, not folded across the call), so the result witnesses
// the real overflow-area placement + read.
//
//   combine reads ONE struct Big {10, 20, 30}: 10 + 20 + 30 = 60 -> exit 60.
//
// Runs natively on the x86_64-Linux CI leg.

struct Big { long a; long b; long c; };   // 24B -> MEMORY class

long combine(int n, ...) {
    va_list ap;
    va_start(ap, n);
    long total = 0;
    for (int i = 0; i < n; i = i + 1) {
        struct Big s = va_arg(ap, struct Big);
        total = total + s.a + s.b + s.c;
    }
    va_end(ap);
    return total;
}

int main(void) {
    struct Big b = {10, 20, 30};
    return (int)combine(1, b);                // 60
}
