// [[D-C-TAG-DEFINED-IN-A-PARAMETER-LIST-REFUSED]] — a function body may declare
// its own `__func__`.
//
// C 6.4.2.2 declares `__func__` implicitly at the top of every function body, the
// same block scope C 6.2.1p4 gives the parameter list, so gcc 13.3.0 and clang
// 18.1.3 refuse this file ("expected identifier or '('" — both treat the name as
// reserved). ✔MEASURED 2026-09-23: MSVC 19.51 builds it and RUNS it to 42 — the
// body's own array is the one named — and one working reference makes the
// program required. It is the second of the two body redeclarations DSS keeps
// accepting while refusing every other redeclaration of what the list declared.
//
// Runtime witness: 42 only if `__func__` names the body's "g", not the
// predefined "f".
int f(void) {
    static const char __func__[] = "g";
    return __func__[0] == 'g' ? 42 : 0;
}

int main(void) { return f(); }
