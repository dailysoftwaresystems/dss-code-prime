// P51, CU B — WEAK fallback definitions of both payloads.
//
// Weak is what makes a lost external definition OBSERVABLE rather than merely
// loud: with cu_a.c's definitions correctly emitted, strong-over-weak resolution
// picks cu_a's bodies and these are inert; with either one wrongly suppressed as
// a C99 6.7.4p7 inline definition, the matching fallback becomes the only
// definition and silently wins. Strong definitions here would collide loudly
// instead, which would hide the quiet failure behind a diagnostic.
//
// The two fallbacks return the SAME value on purpose: 1 and 1 against 40 and 2
// makes every one of the four outcomes a distinct exit code — 42 both correct,
// 3 payload_a lost, 41 payload_b lost, 2 both lost.

__attribute__((weak)) int payload_a(void) { return 1; }
__attribute__((weak)) int payload_b(void) { return 1; }
