struct S { int v; };
int main(void) { struct S s = { 1 }; (struct S){ s }.v; struct S t = (struct S){ s }; t.v = 42; return s.v == 1 ? 42 : 7; }
