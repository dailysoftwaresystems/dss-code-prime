// P68 round 9 (lane `cs`) — the round's P0 merge blocker: an ARRAY PARAMETER's
// adjusted pointer carries ONLY the qualifiers written inside its bracket (C 6.7.6.3p7);
// the element keeps its own.
//
// `O *const objv[]` is a MODIFIABLE `O *const *`, so `objv++` is legal — it is the shape
// sqlite's Tcl test harness is written in (`Tcl_Obj *CONST objv[]` … `objv++`, in
// src/test1.c and ext/session/test_session.c), and DSS refused it S_ConstViolation on
// every leg, so no sqlite testfixture built. ✔MEASURED 2026-09-23, each reference
// separately and every program RUN: gcc 13.3.0, clang 18.1.3, mingw-w64 13.2.0 and MSVC
// 19.51 accept every increment below and run this to 42.
//
// ★ EACH INCREMENT IS OBSERVED: every function reads THROUGH the advanced pointer, so a
// pointer that did not move — or moved by the wrong stride — changes the exit code.
//
// exit 42 = 10 (the element-const pointer array, sqlite's shape behind its macro) + 10
// (`const int p[]`) + 10 (a typedef'd array: its `const` is the ELEMENT's) + 12 (a 2-D
// `const int p[][2]`, stepped by a whole row).

#define CONST const

typedef struct Tcl_Obj { int v; } Tcl_Obj;
typedef int A4[4];

static int tcl_style(int objc, Tcl_Obj *CONST objv[]) {
    objv++;          // skip the command word, as the Tcl commands do
    objc--;
    return objv[0]->v + objc;               // 9 + 1
}

static int const_elements(const int p[]) {
    p++;
    return p[1];                            // 10
}

static int typedef_array(const A4 p) {
    p += 2;
    return p[1];                            // 10
}

static int rows(const int p[][2]) {
    p++;
    return p[0][0] + p[0][1];               // 5 + 7
}

int main(void) {
    Tcl_Obj word = { 0 }, arg = { 9 };
    Tcl_Obj *argv[2] = { &word, &arg };
    int flat[3] = { 1, 2, 10 };
    A4 four = { 0, 0, 0, 10 };
    int grid[2][2] = { { 1, 1 }, { 5, 7 } };
    return tcl_style(2, argv) + const_elements(flat) + typedef_array(four) + rows(grid);
}
