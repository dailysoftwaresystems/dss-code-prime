// P68 round 9 (lane `cs`) — D-C-INCOMPATIBLE-POINTER-CONVERSION-REFUSED-WHERE-EVERY-REFERENCE-WARNS,
// the tag move its closing names: a BARE tag reference in a function DEFINITION's
// parameter list, with no such tag visible, declares the tag in the BODY's block scope
// (C 6.2.1p4) — and the list and the body's outermost block are ONE scope.
//
// ✔MEASURED 2026-09-23, each reference separately, every program RUN:
//   * `g` and `f` below — two definitions each declaring their own `struct Q`, one
//     handing its pointer to the other — gcc 13.3.0, clang 18.1.3 and mingw-w64 13.2.0
//     build them with a "declared inside parameter list" warning and MSVC 19.51
//     silently; all run 42. In C the two `struct Q` are two types, so `g(p)` is an
//     incompatible-pointer conversion, which DSS now admits with its warning — the
//     refusal that kept DSS on MSVC's file-scope reading is gone.
//   * `h` — the body's definition of `struct R` COMPLETES the list's `R`: gcc, clang
//     and mingw build it and run 42; MSVC refuses it (C2037 — on its reading the
//     list's `R` is a file-scope tag that stays incomplete); DSS refused it
//     S_NotAComposite. The recursive `h(&r)` passes a pointer to the body's `R` into
//     the list's `R` WITHOUT a warning: one scope, one type.
//
// exit 42 = f(0) (20 + 2) + h(0) (the completed `R`'s member, 20, plus 22) - 22.

static int g(struct Q *p) { return p == 0 ? 20 : 0; }

static int f(struct Q *p) { return g(p) + 2; }

static int h(struct R *p) {
    struct R { int a; } r = { 20 };
    return p ? p->a : h(&r) + 22;
}

int main(void) { return f(0) + h(0) - 22; }
