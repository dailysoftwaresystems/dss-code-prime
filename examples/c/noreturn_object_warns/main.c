// D-CSUBSET-NORETURN-NON-FUNCTION-OBJECT (C11 6.7.4p2): `_Noreturn` on a
// declaration that does not declare a function. clang and MSVC reject this;
// gcc 13.3.0 ACCEPTS it with "variable 'x' declared '_Noreturn'" and IGNORES
// the specifier — and the disjunction decides acceptance, so DSS does the
// same: warning S_NoreturnNonFunctionObject, specifier dropped, program
// BUILT. x keeps its ordinary tentative zero-initialization, so the exit
// code proves the object was compiled as a plain int. The loop + inlinable
// helper exist for the release arm: mustDifferFromBaseline needs a body the
// pipeline can actually transform.
_Noreturn int x;
static int bump(int v) { return v + x; }
int main(void) {
    int acc = 0;
    for (int i = 0; i < 42; i = i + 1) {
        acc = acc + 1;
    }
    return bump(acc);
}
