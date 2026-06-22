// FC12-deferral④ (D-FC12A-VARIADIC-OVERFLOW-FIXED-STACK-ARGS) FIXED-PARAMS-OVERFLOW
// witness: a SysV variadic callee whose FIXED params overflow the 6 integer arg
// registers onto the incoming stack. This is the case the register-only / common-
// overflow varargs corpora cannot exercise (they all have one fixed param).
//
// `pick(int a..g, ...)`: SysV has 6 integer arg registers (rdi..r9), so the 7 fixed
// ints a..f occupy rdi..r9 and `g` (the 7th) is passed ON THE INCOMING STACK. After
// `va_start`, `overflow_arg_area` must point PAST that named stack arg `g` at the
// FIRST vararg — va_start bakes the fixed-stack-arg byte displacement (1 overflowed
// GPR * 8 = 8) into the VaOverflowArgAreaAddr payload; lir_callconv adds it to the
// overflow base. va_arg(int) then reads the first vararg (100).
//
// main passes pick(1, 2, 3, 4, 5, 6, g, 100): a + g + v = 1 + 7 + 100 = 108 -> exit
// 108. `g` is seeded from a MUTABLE GLOBAL so its load is opaque to ConstFold/Mem2Reg
// (the value cannot be propagated to a const arg slot).
//
// RED-ON-ZERO-DISPLACEMENT: if the fixed-stack-arg displacement is dropped (payload 0
// — overflow_arg_area left at the bare base), va_arg reads the FIXED STACK ARG `g`
// instead of the first vararg → a + g + g = 1 + 7 + 7 = 15, flipping the exit. The
// exit 108 is the run-witness that the displacement threaded end-to-end. Runs natively
// on the x86_64-Linux CI leg.

int g_seed = 7;   // mutable global → opaque load (anti-fold)

int pick(int a, int b, int c, int d, int e, int f, int g, ...) {
    va_list ap;
    va_start(ap, g);
    int v = va_arg(ap, int);   // the FIRST vararg — NOT the fixed stack arg `g`
    va_end(ap);
    return a + g + v;
}

int main(void) {
    return pick(1, 2, 3, 4, 5, 6, g_seed, 100);
}
