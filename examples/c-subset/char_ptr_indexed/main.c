/* D-CSUBSET-CHAR-STRING-VALUE-CODEGEN — the INDEXED byte memory forms:
 * a runtime-index char store/load through writable memory.
 *
 *   box  = 0
 *   q    = (char*)&box   a char* over the int's writable stack bytes
 *   n    = 1             a RUNTIME index (Mem2Reg cannot fold it away)
 *   q[n] = 66            indexed BYTE STORE  (x86 `mov [base+idx*1], r8`)
 *   q[0] = 65            scalar  byte store  (x86 `mov [base], r8`)
 *   return q[n] + q[0]   indexed + scalar BYTE LOAD (movzx) == 66 + 65 == 131
 *
 * This is the one char form that needs base+index addressing — exercised
 * here through (char*)&local writable memory (a string literal is rodata,
 * so an indexed STORE there would fault). x86_64 ONLY: arm64's `lea` has
 * only the ADRP+ADD symbol form (no base+index/base+disp realization), a
 * PRE-EXISTING gap that blocks ALL indexed pointer arithmetic on arm64
 * (int as well as char) — tracked separately, NOT a char-codegen
 * limitation. The non-indexed char forms run on every target in the
 * sibling `char_value` example.
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
