// P68 round 13 (lane `cs`, the static-initializer item) — a WEAK declaration with no
// definition may resolve to a null address when the program loads, so the truth value of its
// address is not known at compile time and is no constant: gcc ("initializer element is not
// computable at load time"), clang and mingw-w64 refuse it in a static initializer. DSS
// refuses it where its static-data producer finds it — H_StaticInitializerNotFolded — and
// never lays it down as a load-time computation.
extern int w __attribute__((weak));
int i = &w != 0;

int main(void) { return i; }
