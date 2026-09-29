/* D-PLAN12-CLOSED-2026-FC7-C1B-COMMIT-B7F547D-FIXED-VIA (fixed via FC7 C1b per-class Arg ordinals): a function with MIXED
 * integer + floating-point parameters must place each in the right register of
 * its OWN class under SysV — the double in xmm0 (FPR ordinal 0), the two ints in
 * rdi/rsi (GPR ordinals 0,1). The pre-fix global counter used the param index as
 * the payload, so the double went to xmm1 and `c` to rdx (both wrong). The
 * values flow through non-inlined mk* so nothing folds. 10 + 20 + 12 = 42.
 * RED-ON-DISABLE: revert the per-class Arg ordinal and the double mis-lands,
 * dropping it off 42. */
int    mix(int a, double b, int c) { return a + (int)b + c; }
double mkd(double v) { return v; }
int    mki(int v)    { return v; }
int main(void) {
    return mix(mki(10), mkd(20.0), mki(12));
}
