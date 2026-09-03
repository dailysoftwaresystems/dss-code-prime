// D-CSUBSET-VLA-PARAM-STAR (C 6.7.6.3p12): the unspecified-size `[*]` array parameter
// is permitted only where the function declarator is NOT part of a definition of that
// function. All three spellings below are definitions and all three are violations;
// the PROTOTYPE at the bottom is the legal form and must draw nothing.
// gcc 13.3.0 and clang 18.1.3 both REFUSE the three definitions and both compile and RUN
// the prototype form; MSVC abstains (no C99 VLA at all).
int f(int n, int a[*]) { return a[0]; }        // named
int g(int, int[*]) { return 3; }               // abstract / unnamed
int h(int a[*][3]) { return a[1][2]; }         // the OUTER dimension

// LEGAL, and must draw nothing: a `[*]` in a NESTED declarator's own prototype, inside
// a definition. Both references compile and run this.
int k(int (*p)(int, int[*])) { return p == 0 ? 0 : 1; }

// LEGAL: `[*]` in a real prototype.
int m(int n, int a[*]);
