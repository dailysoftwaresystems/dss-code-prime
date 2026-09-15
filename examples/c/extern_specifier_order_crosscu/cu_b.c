// P53, CU B — the WEAK fallbacks, the shared object, and the noreturn body.
//
// Weak is what makes a lost external definition OBSERVABLE rather than merely
// loud: with cu_a's reversed-order `inline extern` definitions correctly
// emitted, strong-over-weak resolution picks cu_a's bodies and these are inert;
// with either one wrongly suppressed as a C99 6.7.4p7 inline definition, the
// matching fallback becomes the only definition and silently wins. Strong
// definitions here would COLLIDE loudly instead, which would hide the quiet
// failure behind a diagnostic — the opposite of what this file is for.
//
// The two fallbacks return the SAME value on purpose: 1 and 1 against 33 and 2,
// over a shared 7, makes every outcome a distinct exit code — 42 both correct,
// 10 payload_a lost, 41 payload_b lost, 9 both lost.
//
// `sharedCounter` is DEFINED here and only DECLARED in cu_a. That is the whole
// non-defining assertion: cu_a's `extern int sharedCounter;` must announce a
// name defined elsewhere, not define one of its own.

int sharedCounter = 7;

__attribute__((weak)) int payload_a(void) { return 1; }
__attribute__((weak)) int payload_b(void) { return 1; }

// The `_Noreturn` definition cu_a declares with two leading specifiers. It is
// never reached (cu_a's guard is false), so the loop is a shape, not a hang.
_Noreturn void die(int c) { for (;;) { (void)c; } }
