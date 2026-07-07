// c27 (D-CSUBSET-VOLATILE-POINTEE) — the volatile-pointee STRUCT MEMBER pin (the
// SQLite WAL `volatile u32 **apWiData` shape, scaled down). The member `p` has
// type Ptr<VolatileQual(int)>, so reading `s.p` (the pointer) is a PLAIN load, but
// the DEREF `*s.p` is a VOLATILE access (the optimizer cannot elide/cache/reorder
// it). c21's model B could not express this and failed loud; c27's TYPE qualifier
// carries it. Drives a volatile-pointee member deref through the RELEASE optimizer
// and RUNS it. Returns 42 (a re-read of `*s.p` after a re-assign observes 7).
// (Renamed in place from the retired pointee-reject example `volatile_member_error`.)
struct S { volatile int *p; };

int main(void) {
    int x = 35;
    struct S s;
    s.p = &x;          // a `volatile int *` member
    int a = *s.p;      // volatile load through the member (35)
    *s.p = 7;          // volatile store through the member
    int b = *s.p;      // volatile load — must observe 7, not a cached 35
    return a + b;      // 35 + 7 == 42
}
