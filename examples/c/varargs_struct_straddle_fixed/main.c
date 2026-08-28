// D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS (CLOSED) — the SysV all-or-nothing
// fixed-aggregate-STRADDLE witness. The previous corpus (varargs_overflow_fixed_stack)
// only stacks SCALAR fixed params; this one stacks a by-value AGGREGATE that STRADDLES
// the register/stack boundary, exercising the caller carrier + the callee
// RecvByValueStackParam reception + the va_start displacement together.
//
// `f(long a..e, struct S16 s, ...)`: SysV has 6 integer arg registers (rdi..r9). a..e
// fill rdi..r8 (5 GPRs), leaving only r9; S16 = {long,long} needs 2 GPR eightbytes but
// only 1 remains → SysV places the WHOLE 16-byte struct in MEMORY (all-or-nothing, NEVER
// split across r9 + stack) AND BACKFILLS r9 for the next arg (SysV does not exhaust the
// register class on a stacked aggregate — gcc function_arg_advance_64 leaves cum->nregs).
//
// main calls f(1,2,3,4,5, s={gx,gy}, gv): the first vararg `gv` BACKFILLS into r9, so
// va_start leaves gp_offset = 5*8 = 40 and va_arg(long) reads r9 = gv. The body returns
// a + s.x + s.y + v = 1 + 10 + 20 + 100 = 131 → exit 131. Every value rides a MUTABLE
// GLOBAL so its load is opaque to ConstFold/Mem2Reg (no const arg-slot propagation).
//
// RED-ON-DISABLE (two independent flips, both land on a WRONG exit ≠ 131):
//  * If the struct is SPLIT instead of received whole (revert caller Phase A or callee
//    Phase B), s.x / s.y read garbage / the wrong eightbyte → wrong sum.
//  * If the cursor were CLAMPED like AAPCS64 (gp_offset = 48) instead of backfilled,
//    va_arg(long) would route to the OVERFLOW area and read the struct's first eightbyte
//    (s.x = 10) as the vararg → 1 + 10 + 20 + 10 = 41, not 131.
// Runs natively on the x86_64-Linux CI leg (SysV); the optimized arms re-witness it
// under the real shipped release pipeline (Inlining ON — the FC7-C3 frame-slack lesson).

struct S16 { long x; long y; };

long gx = 10;    // mutable globals → opaque loads (anti-fold)
long gy = 20;
long gv = 100;

long f(long a, long b, long c, long d, long e, struct S16 s, ...) {
    va_list ap;
    va_start(ap, s);
    long v = va_arg(ap, long);   // the FIRST vararg (gv, backfilled in r9) — NOT s's bytes
    va_end(ap);
    return a + s.x + s.y + v;
}

int main(void) {
    struct S16 s;
    s.x = gx;
    s.y = gy;
    return (int)f(1, 2, 3, 4, 5, s, gv);   // 1 + 10 + 20 + 100 = 131
}
