/* c78 (fp_to_ui): `(unsigned)double` — the FPToUI opcode had NO encoding
 * (error[A_NoEncodingDeclared]). x86 CVTTSD2SI (F2 REX.W 0F 2C) reads a value in
 * [0, 2^32) exactly into a signed-64 result whose low 32 bits are the u32 — the
 * same bytes gcc emits (`cvttsd2si rax,xmm0`). c78 gives fp_to_ui the fp_to_si
 * encoding + threads the source float width. `io()` defeats const-fold. 4e9 fits
 * u32 but not i32 (exercises the unsigned path). c92: the arm64 legs land —
 * FCVTZU Xd,Dn (0x9E790000, the FCVTZS mirror; same fixed-X-dest low-32
 * convention; qemu-run on the elf leg). FCVTZU Xd is natively full-range u64,
 * so the >2^63 residual (D-CSUBSET-UI-FROM-FP-UNSIGNED-I64) is x86-ONLY.
 * RED-ON-DISABLE: without the x86 encoding → A_NoEncodingDeclared; without the
 * arm64 block → L_RequiredLirOpcodeMissing fp_to_ui. => 42. */
int io(int x){ return x; }
double g;
int main(void){
    g = (double)io(4000000000);      /* 4e9 — fits u32, not i32 */
    unsigned u = (unsigned)g;        /* 4000000000 */
    return (int)(u % 256u) + 42;     /* 4000000000 % 256 = 0 → 42 */
}
