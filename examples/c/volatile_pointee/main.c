// c27 (D-CSUBSET-VOLATILE-POINTEE) — the pointer-to-volatile-POINTEE pin.
//
// `volatile int *p` was the ONE volatile form c21's model B could not express; it
// failed loud S_VolatilePointeeNotSupported. c27 implements `volatile` as a TYPE
// QUALIFIER (TypeKind::VolatileQual), so `p` has type Ptr<VolatileQual(int)> and
// the DEREF `*p` is a VOLATILE access — the optimizer cannot elide/cache/reorder
// it. This corpus drives a volatile-pointee deref all the way through the RELEASE
// optimizer (Mem2Reg/CSE/DCE) and RUNS it: it pins that a volatile pointee lowers +
// optimizes + executes correctly. The arithmetic is chosen so the program returns
// 42 (a re-read of `*p` after a re-assign must observe 7, not a cached 35).
// (Renamed in place from the retired pointee-reject pin `volatile_error`.)
int main(void) {
    int x = 35;
    volatile int *p = &x;   // pointer-to-volatile-pointee — NOW supported (c27)
    int a = *p;             // volatile load (35)
    *p = 7;                 // volatile store
    int b = *p;             // volatile load — must observe 7, not a cached 35
    return a + b;           // 35 + 7 == 42
}
