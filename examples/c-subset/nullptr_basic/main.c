// C23 nullptr — the runtime witness (D-CSUBSET-NULLPTR / C23 §6.4.4.6, §6.2.5).
// The predefined constant `nullptr` (type nullptr_t) end-to-end: it lowers to the
// target-agnostic integer-0 null constant at the HIR tier (Fix 1(a)), so it reuses
// ALL existing null-pointer-constant lowering and NEVER reaches MIR as a NullptrT.
//
// Exit code = 15 = the OR of 4 independent bits, each a DISTINCT nullptr behavior:
//   1: `void *p = nullptr;`        init a pointer from nullptr, then `p == nullptr`
//   2: `nullptr ? 0 : 2`           nullptr in a controlling expression is false
//   4: `p = opaque; p != nullptr`  a real (opaque) address is not-equal to nullptr
//   8: `p = nullptr; nullptr == p` reassign-to-null + the reversed comparison
//
// The non-null address is taken from a `volatile` source, so the optimizer MUST
// load it at runtime and cannot fold `p != nullptr` to a constant — the release
// arm therefore witnesses a genuine runtime pointer comparison, not a fold. A
// broken nullptr lowering (wrong constant, wrong comparison, a NullptrT reaching
// MIR) cannot produce 15 — it mis-folds a bit or trips the verifier.
int compute(int *opaque) {
    int result = 0;
    void *p = nullptr;                // (1) init a pointer from nullptr
    if (p == nullptr) result |= 1;    //     Ptr == nullptr → true
    result |= (nullptr ? 0 : 2);      // (2) nullptr in a condition → false → 2
    p = opaque;                       // (4) assign an opaque real address
    if (p != nullptr) result |= 4;    //     Ptr != nullptr → true
    p = nullptr;                      // (8) reassign the pointer back to null
    if (nullptr == p) result |= 8;    //     reversed nullptr == Ptr → true
    return result;                    //     1 | 2 | 4 | 8 = 15
}

int main(void) {
    int x = 0;
    int *volatile vp = &x;            // volatile: forces a runtime load, defeats folding
    return compute(vp);
}
