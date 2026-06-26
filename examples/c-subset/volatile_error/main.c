// c21 (D-CSUBSET-VOLATILE-QUALIFIER): repurposed from the retired
// S_VolatileNotSupported wall. `volatile` is now IMPLEMENTED for objects /
// members / pointer-objects (model B: per-symbol isVolatile -> MirInstFlags::
// Volatile). The ONE form model B cannot express is a pointer-to-volatile
// POINTEE (`volatile int *p`) — the volatility would ride the DEREF, needing
// type-level cv-tracking (model A, deferred to D-CSUBSET-VOLATILE-POINTEE / c22).
// It MUST fail loud rather than silently compile `*p` with a non-volatile Load
// the optimizer is free to elide — a silent miscompile. The reject is the
// per-declarator typing arm (a head-position `volatile` + a `*` in the
// declarator); positioned at the declarator `*p` (line 12). A FILE-SCOPE global
// is used so no S_UnusedVariable warning muddies the asserted diagnostic set.
volatile int *p;   // pointer-to-volatile-pointee — NOT supported (model A)

int main(void) {
    return 0;
}
