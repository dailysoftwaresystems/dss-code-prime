/* D-CSUBSET-CHAR-STRING-VALUE-CODEGEN — the INDEXED byte memory forms:
 * a runtime-index char store/load through writable memory.
 *
 *   box  = 0
 *   q    = (char*)&box   a char* over the int's writable stack bytes
 *   n    = 1             a RUNTIME index (Mem2Reg cannot fold it away)
 *   q[n] = 66            indexed BYTE STORE  (x86 mov [base+idx*1],r8 / arm64 STURB)
 *   q[0] = 65            scalar  byte store  (x86 mov [base],r8 / arm64 STURB)
 *   return q[n] + q[0]   indexed + scalar BYTE LOAD (movzx / LDURB) == 66 + 65 == 131
 *
 * This is the one char form that needs base+index addressing — exercised
 * here through (char*)&local writable memory (a string literal is rodata,
 * so an indexed STORE there would fault). The base+index address (`q + n`)
 * lowers to a 4-op `lea` (x86 SIB; arm64 `ADD Xd,Xn,Xm`). arm64 gained that
 * base+index lea this cycle (D-AS4-ARM64-BASE-INDEX-LEA, via the universal
 * width-0 `memoffset.zero` slot), so this example now runs on EVERY target —
 * it was x86-only ONLY because arm64's lea was symbol+disp with no base+index
 * form (a gap that blocked indexed pointer arithmetic for int AND char alike,
 * never a char-codegen limitation).
 *
 * RED-ON-DISABLE: a 64-bit indexed store would clobber box's other 3 bytes
 * and a 64-bit indexed load would read them back — the exit leaves 131. */
int main(void) {
    int box = 0;
    char* q = (char*)&box;
    int n = 1;
    q[n] = 66;
    q[0] = 65;
    return q[n] + q[0];
}
