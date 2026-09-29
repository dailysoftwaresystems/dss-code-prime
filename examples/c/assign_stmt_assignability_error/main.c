// D-SEMANTIC-ASSIGN-STMT-ASSIGNABILITY-BYPASS witness (negative / diagnostic):
// the assignment STATEMENT `p = d;` (int* <- double) must fail loud with the
// SAME positioned S_TypeMismatch the INIT site (`int* p = d;`) already emits.
// Pre-fix the bare assignment ran NO semantic assignability check (only the
// S_ConstViolation check) — type compatibility was enforced only downstream.
// The fix routes the assignment RHS through the SAME shared `isAssignable`
// chokepoint the init / call-arg / return sites use.
//
// THE WITNESS HAS MOVED TWICE, each time because C admits the old pair: first
// `int x; x = f;` (int <- float, an ordinary arithmetic conversion), then
// `int* p; p = q;` with `char* q` — P68 round 9 builds an incompatible pointer
// conversion with the warning every reference gives. A double into a pointer is
// no conversion C has at all: ✔MEASURED 2026-09-23, gcc 13.3.0, clang 18.1.3 and
// mingw-w64 13.2.0 at -std=c17 -pedantic-errors and -std=c2x, and MSVC 19.51 at
// /std:c17 and /std:clatest (C2440), each refuses this file.
//
// `d` is a PARAMETER, so exactly ONE mismatch fires — the `p = d;` statement at
// 26:9 (the RHS). A valid assignment (`p = z;`, int*<-int*) is unaffected.
// Front-end only (semantic-tier), so any single target witnesses it.
// RED-ON-DISABLE: delete the assignment-statement `isAssignable` arm in
// semantic_analyzer.cpp (restore the bypass) -> `p = d;` is silently accepted,
// no S_TypeMismatch fires, and the expect-diagnostics set is empty -> mismatch.
int sink(double d, int* z) {
    int* p;
    p = z;     // valid int*<-int* assignment statement — must stay clean
    p = d;     // invalid int*<-double assignment statement — S_TypeMismatch here
    return *p;
}
int main(void) { return 0; }
