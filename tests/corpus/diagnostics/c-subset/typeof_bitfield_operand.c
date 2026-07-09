struct S { unsigned f : 3; };
struct S s;
typeof(s.f) v;
int main(void) { return 0; }
