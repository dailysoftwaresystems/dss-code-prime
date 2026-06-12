// FC4 c1 stage 2b — the INDIRECT-CALL wall
// (D-CSUBSET-FNPTR-INDIRECT-CALL, high / c2-NEXT).
//
// Declarators TYPE `int (*fp)(int)` and `&helper` materializes the
// pointer (GlobalAddr) — but calling THROUGH the pointer needs the
// call-via-register encoding (LIR call-reg opcode, x86 FF /2, arm64
// BLR; D-ML7-2.4), which c2 ships. Until then the semantic tier walls
// the form LOUDLY with its own positioned code — NOT S_NotCallable
// (the value IS callable in C; the gap is the backend encoding), and
// NOT the LIR tier's late unpositioned L_IndirectCallUnsupported.
//
// The diagnostic lands on the callee identifier: line 16, col 12.
int helper(int v) { return v + 2; }
int main() {
    int (*fp)(int) = &helper;
    return fp(40);
}
