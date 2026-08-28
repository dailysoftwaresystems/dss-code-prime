/* c78 (D-CSUBSET-FLOAT-NEG-ENCODING): floating-point negate `-d`. x86 has no
 * FP-negate instruction; `fneg` had NO encoding (error[A_NoEncodingDeclared]).
 * c78 realizes it capability-dispatched: arm64 uses native FNEG; x86 uses
 * `xorpd/xorps xmm, [rip+mask]` against a 16-byte rodata SIGN-MASK (bit 63 for
 * double / bit 31 for float) — exactly gcc's form. The mask MUST be 16-byte
 * aligned (the legacy XORPD m128 form #GP-faults otherwise); objdump confirms
 * the mask lands 16-aligned on all targets incl. pe64. IEEE-correct: -(+0.0) is
 * -0.0 (not +0.0), so 1.0/-0.0 = -inf. `volatile` defeats const-fold. RED-ON-
 * DISABLE: without the encoding → A_NoEncodingDeclared fneg. => 42. */
volatile double gd = 3.5;
volatile double gz = 0.0;
int main(void){
    double d  = gd;
    double n  = -d;                     /* -3.5 via XORPD [rip+mask] / arm64 FNEG */
    double nz = -gz;                    /* -0.0 */
    int signbit_ok = (1.0 / nz < 0.0);  /* -inf < 0 → signed zero is correct */
    return (int)(n * -12.0) + (signbit_ok ? 0 : 100);   /* -3.5*-12 = 42 */
}
