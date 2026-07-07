// D-SEMANTIC-ASSIGN-STMT-ASSIGNABILITY-BYPASS witness (negative / diagnostic):
// the assignment STATEMENT `p = q;` (int* <- char*, distinct typed pointers) must
// fail loud with the SAME positioned S_TypeMismatch the INIT site (`int* p = q;`)
// already emits. Pre-fix the bare assignment ran NO semantic assignability check
// (only the S_ConstViolation check) — type compatibility was enforced only
// downstream. The fix routes the assignment RHS through the SAME shared
// `isAssignable` chokepoint the init / call-arg / return sites use.
//
// NOTE: this corpus originally used `int x; x = f;` (int <- float), but
// D-CSUBSET-INT-FLOAT-CONVERSION made int<->float an ADMITTED implicit assignment
// conversion in c-subset, so that pair is no longer a mismatch. A distinct-typed-
// pointer pair (`int*` <- `char*`) is the stable always-rejected case that still
// exercises the assignment-statement assignability path; it is NOT a null-pointer
// constant (q is a parameter, not literal 0), so it stays a loud mismatch.
//
// `q` is a PARAMETER, so exactly ONE mismatch fires — the `p = q;` statement at
// 23:9 (the RHS). A valid assignment (`p = z;`, int*<-int*) is unaffected.
// Front-end only (semantic-tier), so any single target witnesses it.
//
// RED-ON-DISABLE: delete the assignment-statement `isAssignable` arm in
// semantic_analyzer.cpp (restore the bypass) -> `p = q;` is silently accepted,
// no S_TypeMismatch fires, and the expect-diagnostics set is empty -> mismatch.
int sink(char* q, int* z) {
    int* p;
    p = z;     // valid int*<-int* assignment statement — must stay clean
    p = q;     // invalid int*<-char* assignment statement — S_TypeMismatch here
    return *p;
}
int main(void) { return 0; }
