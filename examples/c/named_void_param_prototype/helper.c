// The definitions the prototypes in main.c name, declared the ordinary way. A
// function DEFINITION cannot take a parameter of type void (C 6.7.6.3p4 — all four
// references refuse `void f(void v) { }`), so the named-void signature lives only in
// main.c's declarations, and the two translation units meet by name.

int seen;

void f(void) { seen += 1; }

void g(int a) { seen += a; }
