/* c78 (si_to_fp sub-int source): `(double)(signed char / unsigned char / short)`
 * must SExt/ZExt the sub-int source to i32 BEFORE the int→float conversion —
 * CVTSI2SD has only r32/r64 forms (SCVTF only Wn/Xn), so a width-8/16 source hit
 * error[A_NoMatchingEncodingVariant] at the SIToFP. Mirrors gcc's `movsx ecx,cl;
 * cvtsi2sd xmm,ecx`. `io()` defeats const-fold. RED-ON-DISABLE: without the widen
 * → A_NoMatchingEncodingVariant si_to_fp width 8. => 100. */
int io(int x){ return x; }
int main(void){
    signed char   sc = (signed char)io(-7);
    unsigned char uc = (unsigned char)io(200);
    short         sh = (short)io(-300);
    double a = (double)sc;   /* -7.0  (SExt) */
    double b = (double)uc;   /* 200.0 (ZExt) */
    double c = (double)sh;   /* -300.0 (SExt) */
    return (int)(a + b + c) + 207;   /* -107 + 207 = 100 */
}
