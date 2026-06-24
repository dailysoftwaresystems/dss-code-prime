// D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS (CLOSED) — the AAPCS64 all-or-
// nothing fixed-HFA-STRADDLE witness (qemu-aarch64). Mirror of the SysV corpus but for
// the dual-cursor VR class + the AAPCS64 EXHAUST rule (vs SysV backfill).
//
// `f(double a..g (6), struct H3 h, ...)`: AAPCS64 has 8 VR arg registers (v0..v7). The 6
// fixed doubles fill v0..v5, leaving v6,v7 (2 VR); H3 = {double,double,double} is a
// 3-element HFA needing 3 VR but only 2 remain → AAPCS64 §B places the WHOLE 24-byte HFA
// in MEMORY (all-or-nothing, NEVER split) AND EXHAUSTS the VR class (NSRN←8; gcc
// aarch64_layout_arg sets aapcs_nextnvrn = NUM_FP_ARG_REGS). So __vr_offs clamps to 0 and
// the first FP vararg routes to __stack PAST the stacked HFA (overflow base += 24).
//
// main calls f(1..6, h={hx,hy,hz}, vv): va_arg(double) reads vv from the overflow area
// (skipping the 24B HFA). The body returns a + h.a + h.b + h.c + v = 1 + 10 + 20 + 30 +
// 100 = 161 → exit 161. Values ride MUTABLE GLOBALS (opaque to ConstFold/Mem2Reg).
//
// RED-ON-DISABLE (each lands on a WRONG exit ≠ 161):
//  * A SPLIT HFA mis-reads h.a/h.b/h.c.
//  * Reverting the AAPCS64 cursor EXHAUST leaves __vr_offs = -(8-6)*16 = -32: va_arg
//    reads VR slot 6, but the caller (clamp) put vv on the stack → garbage.
//  * A wrong overflow base (no +24 skip) reads the HFA's first double (h.a = 10) as the
//    vararg → 1 + 10 + 20 + 30 + 10 = 71, not 161.
// Runs under qemu-aarch64 (the arm64-Linux CI leg + local build-wsl). The release arm
// re-witnesses under the shipped pipeline with Inlining ON (FC7-C3 lesson).

struct H3 { double a; double b; double c; };

double hx = 10.0;   // mutable globals → opaque loads (anti-fold)
double hy = 20.0;
double hz = 30.0;
double vv = 100.0;

double f(double a, double b, double c, double d, double e, double g,
         struct H3 h, ...) {
    va_list ap;
    va_start(ap, h);
    double v = va_arg(ap, double);   // the FIRST vararg (vv, on the overflow) — NOT h's bytes
    va_end(ap);
    return a + h.a + h.b + h.c + v;
}

int main(void) {
    struct H3 h;
    h.a = hx;
    h.b = hy;
    h.c = hz;
    return (int)f(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, h, vv);   // 1+10+20+30+100 = 161
}
