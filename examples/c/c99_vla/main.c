// D-CSUBSET-VLA C1b: a function-scope variable-length array sized by a RUNTIME
// value RUNS. `volatile` defeats constant-folding, so `n` is genuinely dynamic —
// the array lowers to the dynamic-stack sequence (`sub sp, alignUp(n*4, 16)` + a
// conditional frame pointer that keeps the fixed frame reachable while SP moves),
// NOT a fixed compile-time slot. main is a LEAF function (no calls), which is the
// C1b frame-model scope. A good exercise: a loop index-WRITES every element of the
// runtime-sized array, then two elements are index-READ back.
//
// a[i] = i*7  =>  a[1] = 7, a[5] = 35.  n == 6, so a[n-1] = a[5] = 35.
// Exit code MUST be 35 + 7 = 42. A regression in the dynamic-alloca codegen (the
// arm64 SUB-sp extended-register encoding, the frame-pointer capture/restore, or
// the fixed-frame base-switch) flips the exit code and fails the harness.
int main(void) {
    volatile int seed = 6;   // runtime length; volatile => no constant fold
    int n = seed;
    int a[n];                // VLA: dynamic `sub sp, <aligned n*4>`
    int i;
    for (i = 0; i < n; i = i + 1) {
        a[i] = i * 7;        // index-WRITE across the whole runtime-sized array
    }
    return a[n - 1] + a[1];  // index-READ: 35 + 7 = 42
}
