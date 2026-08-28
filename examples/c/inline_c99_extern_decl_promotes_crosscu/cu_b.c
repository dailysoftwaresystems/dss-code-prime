// TF-C79, CU B — a WEAK fallback definition of `payload`.
//
// Weak is what makes the halfway state OBSERVABLE rather than merely loud: with
// cu_a.c's definition correctly promoted, strong-over-weak resolution picks
// cu_a's body and this one is inert; with cu_a's definition wrongly suppressed,
// this becomes the only definition and silently wins. A strong definition here
// would instead collide loudly, which would hide the quiet failure behind a
// diagnostic.

__attribute__((weak)) int payload(void) { return 1; }
