// D-CSUBSET-VLA C1b CRITICAL-A: a LEAF function that takes a >16-byte by-value
// struct PARAMETER (passed WHOLLY on the incoming stack under SysV's memory class)
// and sizes a variable-length array off one of its members. The parameter's address
// is a FIXED-FRAME reference (`recv_by_value_stack_param`) that, in a VLA function,
// must be taken off the frame pointer, not the runtime-moved SP — the last
// base-switch site. RED-ON-DISABLE for BOTH the SP->FP switch at that site AND the
// completeness verifier: revert the site to SP and the verifier fires (the ref sits
// above the saved-reg window) -> this example fails to COMPILE.
//
// struct Big is 4 + 8*4 = 36 bytes (> 16) -> by-value on the stack. s.n == 6, so the
// VLA has 6 elements; a[i] = i*7 gives a[5] = 35, a[1] = 7.
// Exit code MUST be a[s.n-1] + a[1] = 35 + 7 = 42. LEAF (no calls), non-variadic.
struct Big { int n; int pad[8]; };

int f(struct Big s) {
    int a[s.n];                       // VLA sized by a stack-received struct member
    int i;
    for (i = 0; i < s.n; i = i + 1) {
        a[i] = i * 7;
    }
    return a[s.n - 1] + a[1];         // 35 + 7 = 42
}

int main(void) {
    struct Big b = {6};
    return f(b);
}
