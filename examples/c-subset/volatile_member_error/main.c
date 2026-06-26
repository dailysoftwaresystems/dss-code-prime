// c21 (D-CSUBSET-VOLATILE-QUALIFIER) — the negative golden-diagnostic pin for a
// pointer-to-volatile-POINTEE STRUCT MEMBER. This is the form the CO-LOCATED
// type-resolver arm catches (the member head `typeRefAllowingStruct` greedily
// consumes the `*`, so `volatile int *` is a flat type node with a head-position
// `volatile` BEFORE the star → rejected position-aware), distinct from the
// per-declarator arm that catches object/param/typedef forms. Model B cannot
// express a volatile pointee (it would ride the deref, needing type-level
// cv-tracking — model A / D-CSUBSET-VOLATILE-POINTEE); it MUST fail loud rather
// than silently compile `*s.p` with a non-volatile Load. The reject lands on the
// whole `volatile int *` type node (line 12, col 12); the rejected type resolving
// to InvalidType cascades a secondary S_UnknownType on the field (line 12, col 1).
struct S { volatile int *p; };

int main(void) {
    return 0;
}
