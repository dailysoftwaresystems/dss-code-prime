// D-SEMANTIC-ASSIGN-STMT-ASSIGNABILITY-BYPASS witness (negative / diagnostic):
// the assignment STATEMENT `x = f;` (int <- float) must fail loud with the SAME
// positioned S_TypeMismatch the INIT site (`int x = f;`) already emits. Pre-fix
// the bare assignment ran NO semantic assignability check (only the
// S_ConstViolation check) — type compatibility was enforced only downstream by
// the HIR coerce() (which silently truncated float->int, a correct C VALUE but
// the c-subset "no silent C-style implicit conversion" strictness bypassed). The
// fix routes the assignment RHS through the SAME shared `isAssignable` chokepoint
// the init / call-arg / return sites use, positioned at the RHS.
//
// `f` is a PARAMETER (no narrowing initializer), so exactly ONE mismatch fires —
// the `x = f;` statement at 12:7. A valid assignment (`x = 5;`) is unaffected
// (same int<-int path the four checked sites accept). Front-end only
// (semantic-tier), so any single target witnesses it.
//
// RED-ON-DISABLE: delete the assignment-statement `isAssignable` arm in
// semantic_analyzer.cpp (restore the bypass) -> `x = f;` is silently accepted,
// no S_TypeMismatch fires, and the expect-diagnostics set is empty -> mismatch.
int sink(float f) {
    int x;
    x = 5;     // valid int<-int assignment statement — must stay clean
    x = f;     // invalid int<-float assignment statement — S_TypeMismatch here
    return x;
}
int main(void) { return 0; }
