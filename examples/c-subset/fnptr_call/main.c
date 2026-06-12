// FC4 c2 — the INDIRECT CALL, end-to-end (D-CSUBSET-FNPTR-INDIRECT-CALL
// closed). REPLACES examples/c-subset/fnptr_call_error: the exact source
// that c1 walled with S_IndirectCallNotSupported now compiles AND runs —
// `fp(40)` calls THROUGH the pointer via the call-via-register encoding
// (LIR call-reg variant, x86 FF /2, arm64 BLR).
//
// Exit arithmetic: helper(40) = 40, + 2 = 42. A broken indirect call
// (wrong callee register, clobbered callee, garbage jump) cannot
// produce 42 — it crashes or returns an unrelated value.
int helper(int v) { return v; }
int main() {
    int (*fp)(int) = &helper;
    return fp(40) + 2;
}
