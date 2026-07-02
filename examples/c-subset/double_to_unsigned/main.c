/* c78 (fp_to_ui): `(unsigned)double` — the FPToUI opcode had NO encoding
 * (error[A_NoEncodingDeclared]). x86 CVTTSD2SI (F2 REX.W 0F 2C) reads a value in
 * [0, 2^32) exactly into a signed-64 result whose low 32 bits are the u32 — the
 * same bytes gcc emits (`cvttsd2si rax,xmm0`). c78 gives fp_to_ui the fp_to_si
 * encoding + threads the source float width. `io()` defeats const-fold. 4e9 fits
 * u32 but not i32 (exercises the unsigned path). RED-ON-DISABLE: without the
 * encoding → A_NoEncodingDeclared fp_to_ui. => 42. (arm64 fp_to_ui is a separate
 * future cycle — this example's arm64 leg is not exercised for the cast.) */
int io(int x){ return x; }
double g;
int main(void){
    g = (double)io(4000000000);      /* 4e9 — fits u32, not i32 */
    unsigned u = (unsigned)g;        /* 4000000000 */
    return (int)(u % 256u) + 42;     /* 4000000000 % 256 = 0 → 42 */
}
