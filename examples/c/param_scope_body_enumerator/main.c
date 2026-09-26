// [[D-C-TAG-DEFINED-IN-A-PARAMETER-LIST-REFUSED]] — an ENUMERATOR declared in a
// function body's outermost block may repeat a parameter's name.
//
// C 6.2.1p4 makes a definition's parameter list and its body's outermost block
// ONE scope, so gcc 13.3.0 ("'A' redeclared as different kind of symbol") and
// clang 18.1.3 ("redefinition of 'A'") refuse this file. ✔MEASURED 2026-09-23:
// MSVC 19.51 builds it and RUNS it to 42 — the enumerator hides the parameter in
// the body — and one working reference makes the program required. Every OTHER
// body redeclaration of a parameter (an object, a static, a typedef, an extern
// function) is refused by all four references, and by DSS; this shape and a body
// `__func__` are the two MSVC builds.
//
// Runtime witness: 42 is the enumerator; the argument is 0.
int f(int A) {
    enum E { A = 42 };
    return A;
}

int main(void) { return f(0); }
