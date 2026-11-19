struct S { int v; };
int main(void) { struct S s = { 1 }; struct S *p = &(struct S){ s }; p->v = 42; return s.v == 1 ? 42 : 7; }
