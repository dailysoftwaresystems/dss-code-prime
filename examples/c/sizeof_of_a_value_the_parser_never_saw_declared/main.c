// P68 round 9 (lane `cs`) — `sizeof ( NAME )` where NAME is a VALUE the parser's
// binder sketch never saw declared: the language's predefined identifiers
// (`__func__`, `__FUNCTION__`) and enumeration constants, whose enum lifts them
// into the enclosing scope. Each took the TYPE-NAME reading and was refused
// S_UnknownType; gcc 13.3.0, clang 18.1.3, mingw-w64 13.2.0 and MSVC 19.51 build
// and run every one. The typedef `T5` is the control that must stay a type.
//
// exit = 5 + 5 + 7 + 4 + 4 + 4 + 4 + 5 + 4 = 42 on every target (an `int`
// enumeration constant is 4 bytes on LP64, LLP64 and both arm64 ABIs).

enum { A = 1 };
enum E { B = 2 };
struct S { enum { F = 5 } k; };
typedef char T5[5];

static int helper(void) {
    return (int)sizeof(__func__);            // "helper" + NUL = 7
}

int main(void) {
    enum { C = 3 };
    int total = 0;
    total += (int)sizeof(__func__);          // "main" + NUL = 5
    total += (int)sizeof(__FUNCTION__);      // 5
    total += helper();                       // 7
    total += (int)sizeof(A);                 // 4 — a file-scope anonymous enum
    total += (int)sizeof(B);                 // 4 — a named enum
    total += (int)sizeof(C);                 // 4 — a block-scope enum
    total += (int)sizeof(F);                 // 4 — an enum nested in a struct body
    total += (int)sizeof(T5);                // 5 — the typedef control
    return total + 4;
}
