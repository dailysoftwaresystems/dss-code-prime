// OPT7 weak-inline negative pin (D-OPT7-WEAK-INLINE-NEGATIVE-PIN), CU B.
//
// The STRONG definition of `f`. At link, a strong definition supersedes the
// weak one in cu_a.c (strong-over-weak resolution), so `main`'s call to `f`
// binds to THIS body → exit 42. This is observable ONLY IF cu_a's `main` kept
// the call out-of-line (i.e. the inliner correctly REFUSED to inline the weak
// `f`). If the inliner had baked in cu_a's `return 7`, this definition would
// never be reached and the exit would be 7.
int f() { return 42; }
