/* FC16 D-CSUBSET-ANON-MEMBER-PROMOTION (C11/C23 §6.7.2.1 ¶13): an ANONYMOUS
 * STRUCT member's fields are promoted into the enclosing struct's member
 * namespace with SEQUENTIAL offsets (NOT aliased, unlike an anonymous union).
 * `s.a` @ 0 and `s.b` @ 4 are distinct slots. Write a=40, b=2; return a+b = 42.
 * RED-ON-DISABLE: without promotion s.a/s.b are S0017/undeclared and this does
 * not compile; if a and b ALIASED (union-style), a+b would be 4 (off 42). */
struct S { struct { int a; int b; }; };
int main(void) {
    struct S s;
    s.a = 40;
    s.b = 2;
    return s.a + s.b;   /* 40 + 2 = 42 */
}
