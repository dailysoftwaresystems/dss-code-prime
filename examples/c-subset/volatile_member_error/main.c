// c21 (D-CSUBSET-VOLATILE-QUALIFIER) — the negative golden-diagnostic pin for a
// pointer-to-volatile-POINTEE STRUCT MEMBER. c23 removed the greedy head star
// from `typeRefAllowingStruct`, so a member's `*` now binds per-declarator (like
// every other declaration position); this form is caught by the PER-DECLARATOR
// arm (head `volatile int` + a declarator star `*p`) — the same arm that catches
// object/param/typedef forms. Model B cannot express a volatile pointee (it would
// ride the deref, needing type-level cv-tracking — model A / D-CSUBSET-VOLATILE-
// POINTEE); it MUST fail loud rather than silently compile `*s.p` with a
// non-volatile Load. (Pre-c23 the greedy head star made this a flat type node
// caught by the co-located arm; the per-declarator routing is the cleaner form.)
// The reject lands on the declarator `*p` (line 12, col 25) — one clean diagnostic.
struct S { volatile int *p; };

int main(void) {
    return 0;
}
